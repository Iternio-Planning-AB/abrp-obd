#include <string>
#include <iostream>
#include <vector>

#include "parser.h"

int main() {
  Parser parser;
  double ans{};
  std::string equation{};
  std::vector<int> obdResult{};
  try {
    // for (int i = 0; i <= 78; i++) {
    //   obdResult.push_back(i);
    // }
    // for (auto i : obdResult){
    //   std::cout << i << ", ";
    // }
    std::cout << std::endl;
    auto v = std::vector<int>{ 0x13 };
    equation = "2*!{A:2}*2";

    auto p = parser.parse(equation, v);
    //auto p = parser.parse("int24(1, 1, 1)");
    //auto p = parser.parse("3 + 4 * 2 / ( 1 - 5 ) ^ 2 ^ 3");
    //auto p = parser.parse("{5,2}");
    //auto p = parser.parse("sin ( max ( 2, 3 ) / 3 * 3.14 )");
    if (!p) {
      std::cout << "nullptr ERROR" << std::endl;
    } else {
      ans = p->evaluate();
    }

    printf("Result: %.2f\n", ans);

    std::cout << "Result: " << ans << std::endl;
  }
  catch (const std::exception& e) {
    std::cout << e.what() << std::endl;
  }
}

