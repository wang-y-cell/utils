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

    utils::memory_allocator alloc;
    void* small = alloc.allocate(48);
    void* large = alloc.allocate(alloc.large_threshold() + 64);
    alloc.deallocate(small, 48);
    alloc.deallocate(large, alloc.large_threshold() + 64);

    int* arr = utils::typed_alloc<int>::allocate(8);
    arr[0] = 42;
    utils::typed_alloc<int>::deallocate(arr, 8);
    std::cout << "typed_alloc: ok\n";
}
