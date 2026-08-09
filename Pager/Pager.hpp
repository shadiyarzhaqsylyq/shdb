#ifndef PAGER_HPP
#define PAGER_HPP

#include "BTree.hpp"
#include <cstdint>
#include <string>

class Pager {
private:
    int file_descriptor{-1};
    uint32_t file_length{0};
    uint32_t num_pages{0};
    void* pages[TABLE_MAX_PAGES]{nullptr};

public:
    explicit Pager(const std::string& filename);
    ~Pager();

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    void* get_page(uint32_t page_num);
    void flush(uint32_t page_num);

    uint32_t get_num_pages() const { return num_pages; }
    void set_num_pages(uint32_t n) { num_pages = n; }
    void* get_page_direct(uint32_t i) { return pages[i]; }
    void set_page_direct(uint32_t i, void* ptr) { pages[i] = ptr; }
};

#endif // PAGER_HPP
