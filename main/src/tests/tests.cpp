#include "../frontend/semantics3.cpp"

using namespace semantics;
void test() {
  auto pool = pool_t{};
  auto root = make<ast_t>::call(pool, nullptr, nullptr);
  {
    auto symbols = make<symbols_t>::call(pool, nullptr);
    {
      auto decl1 = alloc_decl(pool, root, symbols, "var");
      auto decl2 = alloc_decl(pool, root, symbols, "var");
      assert(decl1);
      assert(!decl2);
    }
    {
      auto decl1 =
          alloc_decl<redecl_policy::reuse>(pool, root, symbols, "var2");
      auto decl2 =
          alloc_decl<redecl_policy::reuse>(pool, root, symbols, "var2");
      assert(decl1 && decl2);
      assert(decl1 == decl2);
    }
  }
  auto builder = type_builder{pool, root};
  {
    auto arr1 = std::array{
        std::pair{std::string_view("name1"),
                  builder.mutability().immut().real(32)},
        std::pair{std::string_view("name2"),
                  builder.mutability().immut().uint(32)},
        std::pair{std::string_view("name3"),
                  builder.mutability().immut().boolean(32)},
    };
    auto arr2 = std::array{
        std::pair{std::string_view("name1"),
                  builder.mutability().immut().real(32)},
        std::pair{std::string_view("name3"),
                  builder.mutability().immut().boolean(32)},
        std::pair{std::string_view("name2"),
                  builder.mutability().immut().uint(32)},
    };
    auto arr3 = std::array{
        std::pair{std::string_view("name1"),
                  builder.mutability().immut().real(32)},
        std::pair{std::string_view("name5"),
                  builder.mutability().immut().boolean(32)},
        std::pair{std::string_view("name2"),
                  builder.mutability().mut().uint(32)},
    };

    auto variant = builder.mutability().none().variant(arr1);
    auto variant2 = builder.mutability().none().variant(arr1);
    auto variant3 = builder.mutability().none().variant(arr2);
    auto variant4 = builder.mutability().none().variant(arr3);

    assert(type_structs::equals<>::compare(variant, variant2));
    assert(type_structs::equals<>::compare(variant, variant3));
    assert(!type_structs::equals<>::compare(variant, variant4));
  }

  auto type1 = builder.mutability().immut().real(32);
  auto type2 = builder.mutability().mut().real(32);

  auto type3 = remove_mutability(pool, type1);
  auto type4 = remove_mutability(pool, type2);

  auto type5 = builder.mutability().constant().real(32);
  auto type6 = builder.mutability().constant().real(32);
  auto type7 = builder.mutability().none().real(32);

  validator v;
  v.entry(root);

  assert(type_structs::equals<>::compare(type1, type7));
  assert(!type_structs::equals<>::compare(type1, type2));
  assert(type_structs::equals<ignore_mutability_pol>::compare(type1, type2));
  assert(type_structs::equals<>::compare(type3, type4));
  assert(!type_structs::equals<>::compare(type5, type1));
  assert(!type_structs::equals<>::compare(type5, type2));
  assert(type_structs::equals<>::compare(type5, type6));
};
