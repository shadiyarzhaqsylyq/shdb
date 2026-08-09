#include "Pager.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

Pager::Pager(const std::string& filename) {
    file_descriptor = open(filename.c_str(), O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (file_descriptor == -1) {
        std::cout << "Unable to open file\n";
        std::exit(EXIT_FAILURE);
    }

    off_t length = lseek(file_descriptor, 0, SEEK_END);
    file_length = static_cast<uint32_t>(length);
    num_pages = file_length / PAGE_SIZE;

    if (file_length % PAGE_SIZE != 0) {
        std::cout << "Db file is not a whole number of pages. Corrupt file.\n";
        std::exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pages[i] = nullptr;
    }
}

Pager::~Pager() {
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (pages[i] != nullptr) {
            std::free(pages[i]);
            pages[i] = nullptr;
        }
    }
    if (file_descriptor != -1) {
        close(file_descriptor);
    }
}

void* Pager::get_page(uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        std::cout << "Tried to fetch page number out of bounds. " << page_num << " >= " << TABLE_MAX_PAGES << "\n";
        std::exit(EXIT_FAILURE);
    }

    if (pages[page_num] == nullptr) {
        void* page = std::malloc(PAGE_SIZE);
        uint32_t npages = file_length / PAGE_SIZE;
        if (file_length % PAGE_SIZE) {
            npages += 1;
        }
        if (page_num <= npages) {
            lseek(file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(file_descriptor, page, PAGE_SIZE);
            if (bytes_read == -1) {
                std::cout << "Error reading file: " << errno << "\n";
                std::exit(EXIT_FAILURE);
            }
        }
        pages[page_num] = page;
        if (page_num >= num_pages) {
            num_pages = page_num + 1;
        }
    }
    return pages[page_num];
}

void Pager::flush(uint32_t page_num) {
    if (pages[page_num] == nullptr) {
        std::cout << "Tried to flush null page\n";
        std::exit(EXIT_FAILURE);
    }
    off_t offset = lseek(file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
    if (offset == -1) {
        std::cout << "Error seeking: " << errno << "\n";
        std::exit(EXIT_FAILURE);
    }
    ssize_t bytes_written = write(file_descriptor, pages[page_num], PAGE_SIZE);
    if (bytes_written == -1) {
        std::cout << "Error writing: " << errno << "\n";
        std::exit(EXIT_FAILURE);
    }
}
