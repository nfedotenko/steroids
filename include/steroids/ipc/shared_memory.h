/*
 * Copyright(c) 2026 Nikolai Fedotenko.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace steroids {

//------------------------------------------------------------------------------
// RAII named shared-memory segment. create() makes a fresh segment (and
// fails if the name exists), open() attaches to an existing one; both map
// it read-write. The mapping lives until the object is destroyed; the NAME
// lives until unlink() (POSIX) or until the last handle closes (Windows).
// Not a hot-path object — it exists so the channels on top of it can be.
//------------------------------------------------------------------------------
class shared_memory {
public:
    static shared_memory create(const std::string& name, std::size_t size) {
        return shared_memory(name, size, /*create=*/true);
    }

    static shared_memory open(const std::string& name, std::size_t size) {
        return shared_memory(name, size, /*create=*/false);
    }

    // Removes the NAME; existing mappings stay valid until destroyed.
    static void unlink(const std::string& name) noexcept {
#if defined(_WIN32)
        (void)name; // the object dies with its last handle
#else
        ::shm_unlink(normalized(name).c_str());
#endif
    }

    shared_memory(shared_memory&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0))
#if defined(_WIN32)
          ,
          mapping_(std::exchange(other.mapping_, nullptr))
#endif
    {
    }

    shared_memory& operator=(shared_memory&& other) noexcept {
        if (this != &other) {
            release();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
#if defined(_WIN32)
            mapping_ = std::exchange(other.mapping_, nullptr);
#endif
        }
        return *this;
    }

    shared_memory(const shared_memory&) = delete;
    shared_memory& operator=(const shared_memory&) = delete;

    ~shared_memory() { release(); }

    void* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    static std::string normalized(const std::string& name) {
        return name.empty() || name[0] == '/' ? name : "/" + name;
    }

#if defined(_WIN32)
    shared_memory(const std::string& name, std::size_t size, bool create)
        : size_(size) {
        const std::string local = "Local\\" + name;
        if (create) {
            mapping_ = CreateFileMappingA(
                INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                static_cast<DWORD>(size >> 32),
                static_cast<DWORD>(size & 0xFFFFFFFFu), local.c_str());
            if (mapping_ && GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(mapping_);
                mapping_ = nullptr;
            }
        } else {
            mapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE,
                                        local.c_str());
        }
        if (!mapping_) {
            throw std::runtime_error("shared_memory: cannot " +
                                     std::string(create ? "create" : "open") +
                                     " '" + name + "'");
        }
        data_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, size);
        if (!data_) {
            CloseHandle(mapping_);
            throw std::runtime_error("shared_memory: MapViewOfFile failed");
        }
    }

    void release() noexcept {
        if (data_) {
            UnmapViewOfFile(data_);
            data_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }
#else
    shared_memory(const std::string& name, std::size_t size, bool create)
        : size_(size) {
        const std::string n = normalized(name);
        const int flags = create ? (O_RDWR | O_CREAT | O_EXCL) : O_RDWR;
        const int fd = ::shm_open(n.c_str(), flags, 0600);
        if (fd < 0) {
            throw std::runtime_error("shared_memory: cannot " +
                                     std::string(create ? "create" : "open") +
                                     " '" + n + "'");
        }
        if (create && ::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            ::close(fd);
            ::shm_unlink(n.c_str());
            throw std::runtime_error("shared_memory: ftruncate failed");
        }
        data_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, 0);
        ::close(fd); // the mapping keeps the segment alive
        if (data_ == MAP_FAILED) {
            data_ = nullptr;
            if (create) {
                ::shm_unlink(n.c_str());
            }
            throw std::runtime_error("shared_memory: mmap failed");
        }
    }

    void release() noexcept {
        if (data_) {
            ::munmap(data_, size_);
            data_ = nullptr;
        }
    }
#endif

    void* data_{nullptr};
    std::size_t size_{0};
#if defined(_WIN32)
    HANDLE mapping_{nullptr};
#endif
};

} // namespace steroids
