#include "memory/memory.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    utils::memory_pool blocks(sizeof(int), 2);
    void* first = blocks.allocate();
    blocks.deallocate(first);
    void* reused = blocks.allocate();
    assert(reused == first);
    blocks.deallocate(reused);

    utils::object_pool<std::string> strings(2);
    auto text = strings.acquire("pooled object");
    std::cout << *text << ", capacity=" << strings.capacity() << '\n';
}
