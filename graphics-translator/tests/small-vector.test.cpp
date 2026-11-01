#include <cassert>
#include <cstdint>
#include <memory>
#include "../../src/util/util_svector.hpp"

struct Owner {
  int *releases;
  explicit Owner(int *releases) : releases(releases) { }
  ~Owner() { ++*releases; }
};

template<size_t From, size_t To, size_t Count>
void check_move() {
  int releases[Count + 2] = {};
  {
    d3dalloy::small_vector<std::unique_ptr<Owner>, From> source;
    for (size_t i = 0; i < Count; ++i)
      source.emplace_back(std::make_unique<Owner>(&releases[i]));
    auto storage = source.data();
    auto capacity = source.capacity();
    d3dalloy::small_vector<std::unique_ptr<Owner>, To> moved(std::move(source));
    assert(source.empty() && moved.size() == Count);
    if (capacity > std::max(From, To))
      assert(moved.data() == storage && source.capacity() == From);
    else
      assert(moved.data() != storage && source.capacity() == capacity);
    for (size_t i = 0; i < Count; ++i)
      assert(moved[i]->releases == &releases[i] && releases[i] == 0);
    source.emplace_back(std::make_unique<Owner>(&releases[Count]));
    source = std::move(moved);
    assert(releases[Count] == 1 && moved.empty() && source.size() == Count);
    source.erase(source.begin());
    assert(releases[0] == 1);
    source.pop_back();
    assert(releases[Count - 1] == 1);
    source.shrink_to_fit();
    source.clear();
    for (size_t i = 0; i < Count; ++i)
      assert(releases[i] == 1);
    source.emplace_back(std::make_unique<Owner>(&releases[Count + 1]));
  }
  for (int released : releases)
    assert(released == 1);
}

int main() {
  check_move<4, 4, 2>(); // Inline move construction and assignment.
  check_move<1, 1, 5>(); // Heap ownership transfer.
  check_move<4, 1, 3>(); // Inline source spills into smaller destination.
  check_move<1, 8, 3>(); // Heap source fits larger inline destination.
  check_move<1, 2, 5>(); // Heap transfer across inline capacities.

  d3dalloy::small_vector<int, 2> values{1, 2};
  auto inline_storage = values.data();
  assert(values.is_embedded() && values.capacity() == 2);
  values.at(1) = 4;
  const auto &const_values = values;
  assert(const_values.at(0) == 1 && const_values.at(1) == 4);
  values.at(1) = 2;
  values.push_back(3);
  assert(!values.is_embedded() && values.data() != inline_storage);
  assert(values[0] == 1 && values[1] == 2 && values.back() == 3);
  assert(&values.at(2) == &const_values.at(2) && const_values.at(2) == 3);
  auto heap_storage = values.data();
  auto capacity = values.capacity();
  values.clear();
  values.resize(3, 7);
  values.reserve(capacity);
  assert(values.data() == heap_storage && values.capacity() == capacity && values[2] == 7);
  d3dalloy::small_vector<int, 8> copy(values);
  values = copy;
  assert(values.data() == heap_storage && values.size() == 3 && values.front() == 7);
  auto same_capacity_copy = values;
  values = same_capacity_copy;
  assert(values.data() == heap_storage && values.back() == 7);
  values.resize(1);
  values.shrink_to_fit();
  assert(values.is_embedded() && values.data() == inline_storage && values[0] == 7);

  struct alignas(64) Aligned { int value; };
  d3dalloy::small_vector<Aligned, 1> aligned;
  aligned.emplace_back(Aligned{1});
  assert(reinterpret_cast<uintptr_t>(aligned.data()) % alignof(Aligned) == 0);
  aligned.emplace_back(Aligned{2});
  assert(reinterpret_cast<uintptr_t>(aligned.data()) % alignof(Aligned) == 0);
  assert(aligned[0].value == 1 && aligned[1].value == 2);
  aligned.pop_back();
  aligned.shrink_to_fit();
  assert(aligned.is_embedded() && aligned[0].value == 1);
}
