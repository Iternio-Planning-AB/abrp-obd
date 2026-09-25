#include "parser.h"

std::vector<std::string> testsuite(Parser& parser, std::string& equation) {
  double result{};
  std::vector<std::string> errors;
  std::vector<int> obdResult{};

  equation = "-0.01*100";
  result = parser.parse(equation)->evaluate();
  if (result != ( -1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "1";
  result = parser.parse(equation)->evaluate();
  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "1.1";
  result = parser.parse(equation)->evaluate();
  if (result != ( 1.1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "1 + 2";
  result = parser.parse(equation)->evaluate();
  if (result != ( 3 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }



  equation = "1 + 2 * 3";
  result = parser.parse(equation)->evaluate();
  if (result != ( 7 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "3 * 1 + 2";
  result = parser.parse(equation)->evaluate();
  if (result != ( 5 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "{9 : 1}";
  result = parser.parse(equation)->evaluate();
  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "2 - 1";
  result = parser.parse(equation)->evaluate();
  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "1 << 8";
  result = parser.parse(equation)->evaluate();
  if (result != ( 256 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "256 >> 8";
  result = parser.parse(equation)->evaluate();
  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "1 << 8 + 2";
  result = parser.parse(equation)->evaluate();
  if (result != ( 258 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "2 + 1 << 8";
  result = parser.parse(equation)->evaluate();
  if (result != ( 258 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "1 << 8 * 2";
  result = parser.parse(equation)->evaluate();
  if (result != ( 512 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "2 * 1 << 8";
  result = parser.parse(equation)->evaluate();
  if (result != ( 512 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "256 > 8";
  result = parser.parse(equation)->evaluate();
  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "1 < 8 + 2";
  result = parser.parse(equation)->evaluate();
  if (result != ( 258 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "2 + 1 < 8";
  result = parser.parse(equation)->evaluate();
  if (result != ( 258 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "1 < 8 * 2";
  result = parser.parse(equation)->evaluate();
  if (result != ( 512 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "((1<<16+2<<8+3)/64)-1200";
  result = parser.parse(equation)->evaluate();
  if (result != ( -167.953125 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "signed(5)";
  result = parser.parse(equation)->evaluate();
  if (result != ( 5 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "signed(129)";
  result = parser.parse(equation)->evaluate();
  if (result != ( -127 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "signed(255)";
  result = parser.parse(equation)->evaluate();
  if (result != ( -1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "signed(0)";
  result = parser.parse(equation)->evaluate();
  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "signed(A)";
  result = parser.parse(equation, { 0x00 })->evaluate();
  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "signed(A)";
  result = parser.parse(equation, { 0x81 })->evaluate();
  if (result != ( -127 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "signed(A)";
  result = parser.parse(equation, { 0xff })->evaluate();
  if (result != ( -1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "A";
  result = parser.parse(equation, { 0x00 })->evaluate();
  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "A + B";
  result = parser.parse(equation, { 0x00, 0x01 })->evaluate();
  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "A << 8";
  result = parser.parse(equation, { 0x01 })->evaluate();
  if (result != ( 256 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "int16(A:B)";
  result = parser.parse(equation, { 0x01, 0x05 })->evaluate();
  if (result != ( 0x01 * 256 + 5 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "int24(A:B:C)";
  result = parser.parse(equation, { 0x05, 0x04, 0x03 })->evaluate();
  if (result != ( 0x05 * 65536 + 0x04 * 256 + 0x03 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  equation = "((signed(0)*256)+11)/10";
  result = parser.parse(equation)->evaluate();
  if (result != ( 1.1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  // // binary value is 00000011
  equation = "{A:0} + {A:1} + {A:2} + {A:3} + {A:4} + {A:5} + {A:6} + {A:7}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 2 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "{A:0}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "{A:1}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "{A:2}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  // binary value is 00000011
  equation = "!{A:0}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "!{A:2}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "!{A:2}*2";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 2 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "2*!{A:2}*2";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 4 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  // binary value is 00000011
  equation = "{A:0}&&{A:1}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "{A:0}&&{A:2}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  // binary value is 00000011
  equation = "{A:0}||{A:1}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "{A:0}||{A:2}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "{A:2}||{A:3}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  // binary value is 00000011
  // 1 && 0 || 1
  equation = "{A:0}&&{A:2}||{A:1}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  // 0 || 0 && 1
  equation = "{A:2}||{A:3}&&{A:0}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  // 0 || 1 && 1
  equation = "{A:2}||{A:0}&&{A:1}";
  result = parser.parse(equation, { 0x03 })->evaluate();

  if (result != ( 1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }

  equation = "ifGreaterThan(A;127;A-256;A)";
  result = parser.parse(equation, { 0x13 })->evaluate();

  if (result != ( 19 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }
  equation = "ifGreaterThan(A;127;A-256;A)";
  result = parser.parse(equation, { 0xff })->evaluate();

  if (result != ( -1 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  for (int i = 0; i <= 78; i++) {
    obdResult.push_back(i);
  }

  equation = "VariableTest";

  result = parser.parse("A", { obdResult })->evaluate();
  if (result != ( 0 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }
  result = parser.parse("C", { obdResult })->evaluate();
  if (result != ( 2 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }
  result = parser.parse("Z", { obdResult })->evaluate();
  if (result != ( 25 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }
  result = parser.parse("AA", { obdResult })->evaluate();
  if (result != ( 26 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }
  result = parser.parse("AF", { obdResult })->evaluate();
  if (result != ( 31 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }
  result = parser.parse("AZ", { obdResult })->evaluate();
  if (result != ( 51 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }
  result = parser.parse("BA", { obdResult })->evaluate();
  if (result != ( 52 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }
  result = parser.parse("CA", { obdResult })->evaluate();
  if (result != ( 78 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }
  result = parser.parse("AD", { obdResult })->evaluate();
  if (result != ( 29 )) {
    errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  }


  //   equation = "VAL{ABC}*5", {
  //     val: { ABC: 5 },

  //     if (result != ( 25 )) {
  //       errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  //     }


  //       equation = 
  //       "-1.63863194e-12*(VAL{soc}^6) + 1.28552286e-9*(VAL{soc}^5) - 3.77151297e-7*(VAL{soc}^4) + 4.88813535e-5*(VAL{soc}^3) - 2.15195123e-3*(VAL{soc}^2) + 0.388970146*VAL{soc} - 4.00976479",
  //       { val: { soc: 241 } }
  //     );
  //     if (result != ( 100.738304420554 )) {
  //       errors.push_back(std::string("Error at: " + equation) + std::string(" : " + std::to_string(result)));
  //     }
  //   }

  return errors;
}

int main() {
  Parser p;
  std::string equation;
  // std::vector<std::string> errors;

  try {
    auto errors = testsuite(p, equation);
    auto size = errors.size();
    if (size) {
      std::cout << "ERROR: " << size << " tests failed!" << std::endl;
      std::cout << "Stack: " << std::endl;
      for (size_t i = 0; i < size; i++) {
        std::cout << errors[i] << std::endl;
      }
    } else {
      std::cout << "ALL TESTS PASSED" << std::endl;
    }
  }
  catch (const std::exception& e) {
    std::cout << "Failed catastrophically at: '" << equation << "'. A standard exception was caught, with message: '"
      << e.what() << "'" << std::endl;
  }
}
