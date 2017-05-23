#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <string>
#include <set>
#include <cassert>
#include <chrono>
#include <queue>

#define USE_MPI

#ifdef USE_MPI
#include <mpi.h>
#endif

namespace {

struct KokkosPDeviceInfo {
  std::uint32_t deviceID;
};

struct SpaceHandle {
  char name[64];                                                                        
};

#if 0
enum Space {
  SPACE_HOST,
  SPACE_CUDA
};

enum Space get_space(SpaceHandle const& handle) {
  if (handle.name[0] == 'H') return SPACE_HOST;
  if (handle.name[0] == 'C') return SPACE_CUDA;
  abort();
  return SPACE_HOST;
}
#endif

struct Now {
  typedef std::chrono::time_point<std::chrono::high_resolution_clock> Impl;
  Impl impl;
};

Now now() {
  Now t;
  t.impl = std::chrono::high_resolution_clock::now();
  return t;
}

double operator-(Now b, Now a) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(b.impl - a.impl)
             .count() *
         1e-9;
}

enum StackKind {
  STACK_FOR,
  STACK_REDUCE,
  STACK_SCAN,
  STACK_REGION
};

struct StackNode {
  StackNode* parent;
  std::string name;
  StackKind kind;
  std::set<StackNode> children;
  double total_runtime;
#ifdef USE_MPI
  double mpi_max_runtime;
#endif
  std::int64_t number_of_calls;
  Now start_time;
  StackNode(StackNode* parent_in, std::string&& name_in, StackKind kind_in):
    parent(parent_in),
    name(std::move(name_in)),
    kind(kind_in),
    total_runtime(0.0),
    number_of_calls(0) {
  }
  StackNode* get_child(std::string&& child_name, StackKind child_kind) {
    StackNode candidate(this, std::move(child_name), child_kind);
    auto it = children.find(candidate);
    if (it == children.end()) {
      auto res = children.emplace(std::move(candidate));
      it = res.first;
      assert(res.second);
    }
    return const_cast<StackNode*>(&(*(it)));
  }
  bool operator<(StackNode const& other) const {
    if (this->kind != other.kind) {
      return int(this->kind) < int(other.kind);
    }
    return this->name < other.name;
  }
  std::string get_full_name() const {
    std::string full_name;
    for (auto p = this; p; p = p->parent) {
      full_name = p->name + '/' + full_name;
    }
    return full_name;
  }
  void begin() {
    this->number_of_calls++;
    this->start_time = now();
  }
  void end(Now const& end_time) {
    auto runtime = (end_time - this->start_time);
    this->total_runtime += runtime;
  }
  StackNode invert() const {
    StackNode inv_root(nullptr, "", STACK_REGION);
    std::queue<StackNode const*> q;
    q.push(this);
    while (!q.empty()) {
      auto node = q.front(); q.pop();
      auto self_time = node->total_runtime;
      auto calls = node->number_of_calls;
      for (auto& child : node->children) {
        self_time -= child.total_runtime;
        q.push(&child);
      }
      auto inv_node = &inv_root;
      inv_node->total_runtime += self_time;
      inv_node->number_of_calls += calls;
      for (; node; node = node->parent) {
        std::string name = node->name;
        inv_node = inv_node->get_child(std::move(name), node->kind);
        inv_node->total_runtime += self_time;
        inv_node->number_of_calls += calls;
      }
    }
    return inv_root;
  }
  void print_recursive(
      std::ostream& os, std::string my_indent, std::string const& child_indent, double tree_time) const {
    auto percent = int((this->total_runtime / tree_time) * 100.0);
    if (percent < 1) return; // 1% of total runtime is noise threshold
    if (!this->name.empty()) {
      os << my_indent;
      os << percent << "% " << this->number_of_calls << " " << this->name;
      switch (this->kind) {
        case STACK_FOR: os << " [for]"; break;
        case STACK_REDUCE: os << " [reduce]"; break;
        case STACK_SCAN: os << " [scan]"; break;
        case STACK_REGION: os << " [region]"; break;
      };
      os << '\n';
    }
    if (this->children.empty()) return;
    auto by_time = [](StackNode const* a, StackNode const* b) {
      if (a->total_runtime != b->total_runtime) {
        return a->total_runtime > b->total_runtime;
      }
      return a->name < b->name;
    };
    std::set<StackNode const*, decltype(by_time)> children_by_time(by_time);
    for (auto& child : children) {
      children_by_time.insert(&child);
    }
    auto last = children_by_time.end();
    --last;
    for (auto it = children_by_time.begin(); it != children_by_time.end(); ++it) {
      std::string grandchild_indent;
      if (it == last) {
        grandchild_indent = child_indent + "    ";
      } else {
        grandchild_indent = child_indent + "|   ";
      }
      auto child = *it;
      child->print_recursive(
          os, child_indent + "|-> ", grandchild_indent, tree_time);
    }
  }
  void print(std::ostream& os) const {
    this->print_recursive(os, "", "", this->total_runtime);
    os << '\n';
  }
#ifdef USE_MPI
  void reduce_over_mpi() {
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    std::queue<StackNode*> q;
    q.push(this);
    while (!q.empty()) {
      auto node = q.front(); q.pop();
      node->mpi_max_runtime = node->total_runtime;
      MPI_Allreduce(MPI_IN_PLACE, &(node->total_runtime),
          1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
      MPI_Allreduce(MPI_IN_PLACE, &(node->mpi_max_runtime),
          1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
      /* There may be kernels that were called on rank 0 but were not
         called on certain other ranks.
         We will count these and add empty entries in the other ranks.
         There may also be kernels which were *not* called on rank 0 but
         *were* called on certain other ranks.
         We are *ignoring* these, because I can't think of an easy and
         scalable way to combine that data */
      int nchildren = int(node->children.size());
      MPI_Bcast(&nchildren, 1, MPI_INT, 0, MPI_COMM_WORLD);
      if (rank == 0) {
        for (auto& child : node->children) {
          int name_len = int(child.name.length());
          MPI_Bcast(&name_len, 1, MPI_INT, 0, MPI_COMM_WORLD);
          auto name = child.name;
          MPI_Bcast(&name[0], name_len, MPI_CHAR, 0, MPI_COMM_WORLD);
          int kind = child.kind;
          MPI_Bcast(&kind, 1, MPI_INT, 0, MPI_COMM_WORLD);
          q.push(const_cast<StackNode*>(&child));
        }
      } else {
        for (int i = 0; i < nchildren; ++i) {
          int name_len;
          MPI_Bcast(&name_len, 1, MPI_INT, 0, MPI_COMM_WORLD);
          std::string name(size_t(name_len), '?');
          MPI_Bcast(&name[0], name_len, MPI_CHAR, 0, MPI_COMM_WORLD);
          int kind;
          MPI_Bcast(&kind, 1, MPI_INT, 0, MPI_COMM_WORLD);
          auto child = node->get_child(std::move(name), StackKind(kind));
          q.push(child);
        }
      }
    }
  }
#endif
};

struct State {
  StackNode stack_root;
  StackNode* stack_frame;
  State():stack_root(nullptr, "", STACK_REGION),stack_frame(&stack_root) {
    stack_frame->begin();
  }
  ~State() {
    auto end_time = now();
    if (stack_frame != &stack_root) {
      std::cerr << "Program ended before \"" << stack_frame->get_full_name()
                << "\" ended\n";
      abort();
    }
    stack_frame->end(end_time);
#ifdef USE_MPI
    stack_root.reduce_over_mpi();
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank == 0)
#endif
    {
      std::cout << "\nBEGIN KOKKOS PROFILING REPORT:\n";
#ifdef USE_MPI
      std::cout << "TOTAL TIME: " << stack_root.mpi_max_runtime << " seconds\n";
#else
      std::cout << "TOTAL TIME: " << stack_root.total_runtime << " seconds\n";
#endif
      std::cout << "TOP-DOWN TIME TREE:\n";
      std::cout << "================== \n";
      stack_root.print(std::cout);
      auto inv_stack_root = stack_root.invert();
      std::cout << "BOTTOM-UP TIME TREE:\n";
      std::cout << "=================== \n";
      inv_stack_root.print(std::cout);
      std::cout << "END KOKKOS PROFILING REPORT.\n";
    }
    MPI_Barrier(MPI_COMM_WORLD);
  }
  std::uint64_t begin_kernel(const char* name, StackKind kind) {
    std::string name_str(name);
    stack_frame = stack_frame->get_child(std::move(name_str), kind);
    stack_frame->begin();
    return reinterpret_cast<std::uint64_t>(stack_frame);
  }
  void end_kernel(std::uint64_t kernid) {
    auto end_time = now();
    auto expect_node = reinterpret_cast<StackNode*>(kernid);
    if (expect_node != stack_frame) {
      std::cerr << "Expected \"" << stack_frame->get_full_name()
                << "\" to end, got different kernel ID\n";
      abort();
    }
    stack_frame->end(end_time);
    stack_frame = stack_frame->parent;
  }
  void push_region(const char* name) {
    std::string name_str(name);
    stack_frame = stack_frame->get_child(std::move(name_str), STACK_REGION);
    stack_frame->begin();
  }
  void pop_region() {
    stack_frame->end(now());
    stack_frame = stack_frame->parent;
  }
};

State* global_state = nullptr;

}  // end anonymous namespace

extern "C" void kokkosp_init_library(
    int loadseq, uint64_t version, uint32_t ndevinfos, KokkosPDeviceInfo* devinfos) {
  (void)loadseq;
  (void)ndevinfos;
  (void)devinfos;
  if (version != 20150628) {
    std::cerr << "kokkosp_init_library: version " << version << " != 20150628\n";
    abort();
  }
  global_state = new State();
}

extern "C" void kokkosp_begin_parallel_for(
    const char* name, std::uint32_t devid, std::uint64_t* kernid) {
  (void) devid;
  *kernid = global_state->begin_kernel(name, STACK_FOR);
}

extern "C" void kokkosp_begin_parallel_reduce(
    const char* name, std::uint32_t devid, std::uint64_t* kernid) {
  (void) devid;
  *kernid = global_state->begin_kernel(name, STACK_REDUCE);
}

extern "C" void kokkosp_begin_parallel_scan(
    const char* name, std::uint32_t devid, std::uint64_t* kernid) {
  (void) devid;
  *kernid = global_state->begin_kernel(name, STACK_SCAN);
}

extern "C" void kokkosp_end_parallel_for(std::uint64_t kernid) {
  global_state->end_kernel(kernid);
}

extern "C" void kokkosp_end_parallel_reduce(std::uint64_t kernid) {
  global_state->end_kernel(kernid);
}

extern "C" void kokkosp_end_parallel_scan(std::uint64_t kernid) {
  global_state->end_kernel(kernid);
}

extern "C" void kokkosp_push_profile_region(const char* name) {
  global_state->push_region(name);
}

extern "C" void kokkosp_pop_profile_region() {
  global_state->pop_region();
}

extern "C" void kokkosp_finalize_library() {
  delete global_state;
  global_state = nullptr;
}
