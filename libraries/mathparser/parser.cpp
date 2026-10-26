/**
 * @file parser.cpp
 * @author Fatih Küçükkarakurt (https://github.com/fkkarakurt)
 *
 * @copyright Copyright (c) 2023
 *
 * Expanded by Ludvig Delvret for Iternio
 * 
 */

#define PRINT 0
#define PRINT2 0

#include "parser.h"
#include <vector>
#include <regex>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>


static std::string lower(std::string s) {
  for (auto& c : s) c = tolower((unsigned char)c);
  return s;
}

std::vector<Token> Parser::tokenize(const std::string& expression, const std::vector<int>& variables,
                                    const ParserValues* values) {
  std::vector<Token> tokens;
  std::string currentToken;
  std::cmatch match;

  for (size_t i = 0; i < expression.size(); ++i) {
    char ch = expression[i];
    auto str1 = std::string(expression.substr(i, 1));
    auto str2 = std::string(expression.substr(i, 2));
    auto str3 = std::string(expression.substr(i, 3));
    auto str4 = std::string(expression.substr(i, 4));
    auto str5 = std::string(expression.substr(i, 5));
    auto str6 = std::string(expression.substr(i, 6));
    auto str13 = std::string(expression.substr(i, 13));


    if (isspace(ch)) {
      if (!currentToken.empty()) {
        tokens.emplace_back(currentToken, Token::NUM);
        currentToken.clear();
      }
      continue;
    }

#if PRINT
    // for (auto token : tokens) {
    //   std::cout << token.expr << " ";
    // }
    std::cout << ": " <<
      currentToken <<
      " : " << expression.substr(i) << " : " <<
      std::endl;
    std::cout << "ch: " << ch << std::endl;

#endif

    if (ch == '-') {
      // Decide whether this '-' is a unary sign (start of a negative number)
      // or a binary subtraction operator.
      // It is unary when it is the very first token, or when the previously
      // emitted token is an operator that is NOT a closing parenthesis
      // (e.g. the '-' in "*-1", "(-1", "+-1"). Otherwise it is binary.
      bool unaryMinus;
      if (!currentToken.empty()) {
        unaryMinus = false;                 // e.g. "12-" -> binary subtraction
      } else if (tokens.empty()) {
        unaryMinus = true;                  // leading minus
      } else {
        unaryMinus = ( tokens.back().type == Token::OP && tokens.back().expr != ")" );
      }
      if (unaryMinus) {
        currentToken += ch;                 // begin a negative-number literal
        continue;
      }
      // else: fall through; the operator branch below emits a binary '-'
    }

    // VAL{name}: another key's value from this poll, as the app's virtual keys use. The app's grammar takes the
    // name's characters from [a-zA-z0-9], a range that includes '_' and so the profiles' _-prefixed keys.
    if (lower(str4) == "val{") {
      const size_t close = expression.find('}', i + 4);
      if (close == std::string::npos) throw std::runtime_error("Unclosed VAL{");
      const std::string name = expression.substr(i + 4, close - i - 4);
      const ParserValues::value_type* found = nullptr;
      if (values)
        for (const auto& v : *values) if (v.first == name) found = &v;
      if (!found) throw std::runtime_error("val not available: " + name);
      if (!currentToken.empty() && currentToken != "-") throw std::runtime_error("Number before VAL{");
      char number[32];
      snprintf(number, sizeof(number), "%.17g", currentToken == "-" ? -found->second : found->second);
      tokens.emplace_back(number, Token::NUM);
      currentToken.clear();
      i = close;
      continue;
    }

    // Function names in any case, as the app's grammar takes them (MEB writes ifgreaterthan).
    if (lower(str13) == "ifgreaterthan") {
      tokens.emplace_back("ifGreaterThan", Token::FUNC);
      // skip 13 characters
      i += 12;
      continue;
    }

    {
      std::string str6lower = str6;
      for (auto& c : str6lower) c = tolower(c);
      if (str6lower == "signed") {
        tokens.emplace_back("signed", Token::FUNC);
        // skip 6 characters
        i += 5;
        continue;
      }
    }

    if (lower(str5) == "int32" || lower(str5) == "int24" || lower(str5) == "int16") {
      tokens.emplace_back(lower(str5), Token::FUNC);

      // skip 5 characters
      i += 4;
      continue;
    }

    if (str4 == "sinh" || str4 == "cosh" ||
      str4 == "tanh" || str4 == "coth" ||
      str4 == "sech" || str4 == "csch" ||
      str4 == "sqrt") {
      tokens.emplace_back(str4, Token::FUNC);

      // skip 4 characters
      i += 3;
      continue;
    }

    else if (str3 == "sin" || str3 == "cos" ||
      str3 == "tan" || str3 == "cot" ||
      str3 == "max" || str3 == "log") {
      tokens.emplace_back(str3, Token::FUNC);

      // skip 3 characters
      i += 2;
      continue;
    } else if (str2 == "ln") {
      tokens.emplace_back("ln", Token::FUNC);

      // Since 'ln' is two characters long, an extra increment.
      i += 1;
    }

    else if (isdigit(ch) || ch == '.') {
      currentToken += ch;
    }
    //Expand to handle variables
    else if (isalpha(ch) && std::regex_search(str3.c_str(), match, std::regex("[a-zA-Z]+"))) { //support for a-ZZZ

      std::string variable = match[0].str();
      std::string str;
      int index{ 0 };

      for (auto& c : variable) {
        c = tolower(c);
      }

      if (variable.length() == 1) {
        index = variable[0] - 'a';
      } else {
        for (int i{ 0 }; i < variable.length(); ++i) {
          auto c = variable[i] - 'a';
          if (i == 0) {
            index += 26 + c * 26;
          } else {
            index += c + ( i - 1 ) * 26;
          }
        }
        i += variable.length() - 1;
      }
      // Bounds-check BOTH branches before indexing. Single-letter variables (A,B,C...) were unchecked: a
      // short/garbage OBD response yields fewer variables than the equation references (e.g. INT24(A:B:C)
      // against a 1-byte buffer), and variables[index] read out of bounds -> junk results (-2.9e8) and, on an
      // empty response, heap-corrupting UB that abort()ed the chip (the PANIC reboots). The old multi-letter
      // check also underflowed on an empty vector (`index > variables.size()-1` with unsigned size()==0).
      // A byte the reply does not have is NaN, as the app's undefined is: ifgreaterthan(C+1, 0, ...) then takes its
      // else branch, which is how MEB's _soe_safe falls back when only the shorter of its two replies came. Any
      // other use stays NaN, and readPIDs drops a non-finite result.
      if (index < 0 || (size_t)index >= variables.size()) {
        str = "nan";
      } else {
        str = std::to_string(variables[index]);
      }

      if (currentToken.length() == 0) {
        tokens.emplace_back(str, Token::NUM);
      } else if (currentToken.back() == '-') {
        currentToken += str;
        tokens.emplace_back(currentToken, Token::NUM);
        currentToken.clear();
      }
    } else if ( // Implement support for bitshifts
      str2 == ">>" || str2 == "<<" ||
      str2 == "&&" || str2 == "||" ||
      str2 == "==" || str2 == "!=") {
      if (!currentToken.empty()) {
        tokens.emplace_back(currentToken, Token::NUM);
        currentToken.clear();
      }
      // The app has no !=; refusing it drops the key there and here alike.
      if (str2 == "!=") throw std::runtime_error("Unsupported operator: !=");
      // == binds as * does in the app's grammar.
      tokens.emplace_back(str2, Token::OP, str2 == "==" ? 3 : 5);
      //An extra increment for 2-char tokens
      ++i;
    } else {
      if (!currentToken.empty()) {
        tokens.emplace_back(currentToken, Token::NUM);
        currentToken.clear();
      } if (ch == '+' || ch == '-') tokens.emplace_back(str1, Token::OP, 2);
      else if (ch == '/' || ch == '*') tokens.emplace_back(str1, Token::OP, 3);
      else if (ch == '^') tokens.emplace_back(str1, Token::OP, 4, Token::RIGHT);
      else if (ch == ':' || ch == ',' || ch == ';') { //Used as number seperator func(A:B) == func(A,B);
        tokens.emplace_back(")", Token::OP);
        tokens.emplace_back("(", Token::OP); // Hack for handling variables
      } else if (ch == '(' || ch == ')') {
        tokens.emplace_back(str1, Token::OP);
        tokens.emplace_back(str1, Token::OP);
      } else if (ch == '<' || ch == '>') tokens.emplace_back(str1, Token::OP, 5);
      else if (ch == '{') { //create a function bit -> {a:b} == bit(a,b)
        tokens.emplace_back("bit", Token::FUNC);
        tokens.emplace_back("(", Token::OP);
        tokens.emplace_back("(", Token::OP); //Hack for handling variables
      } else if (ch == '}') {
        tokens.emplace_back(")", Token::OP);
        tokens.emplace_back(")", Token::OP); //Hack for handling variables
      } else if (ch = '!') {
        tokens.emplace_back("not", Token::FUNC);
      } else {
        throw std::runtime_error("Unknown character: " + ch);
      }
    }
  }

  // Add the last token
  if (!currentToken.empty()) {
    tokens.emplace_back(currentToken, Token::NUM);
  }
  // std::cout << "tokenizer done" << std::endl;

  return tokens;
}


std::vector<Token> Parser::buildStack(const std::vector<Token>& tokens) {

  std::vector<Token> outputStack;
  std::vector<Token> operatorStack;

  for (size_t i = 0; i < tokens.size(); ++i) {

#if PRINT2
    std::cout << "operatorStack:  ";
    for (auto token : operatorStack) {
      std::cout << token.expr << " ";
    }
    std::cout << std::endl;
    std::cout << "outputStack:    ";
    for (auto token : outputStack) {
      std::cout << token.expr << " ";
    }
    std::cout << std::endl;
    std::cout << "next token:     " << tokens[i].expr << std::endl;
    std::cout << std::endl;

#endif

    const auto& token = tokens[i];
    if (token.type == Token::NUM) {
      outputStack.push_back(token);
    } else if (token.type == Token::FUNC) {
      operatorStack.push_back(token);
    } else if (token.type == Token::OP) {
      if (token.expr == "(") {
        operatorStack.push_back(token);
      } else if (token.expr == ":") {
        auto back = operatorStack.back();
        while (back.expr != "(") {
          outputStack.push_back(back);
          operatorStack.pop_back();
          back = operatorStack.back();
          operatorStack.push_back(token);
        }
      } else if (token.expr == ")") {
        auto back = operatorStack.back();
        while (back.expr != "(") {
          if (operatorStack.empty()) {
            throw std::runtime_error("Mismatched parenthesis");
          }
          outputStack.push_back(back);
          operatorStack.pop_back();
          back = operatorStack.back();
        }
        if (back.expr != "(") {
          throw std::runtime_error("Expected '(', got: " + back.expr);
        }
        operatorStack.pop_back(); //pop and discard left parenthesis
        if (!operatorStack.empty()) {

          back = operatorStack.back();
          if (back.type == Token::FUNC) {
            outputStack.push_back(operatorStack.back());
            operatorStack.pop_back();
          }
        }
      } else if (!operatorStack.empty()) {

        auto back = operatorStack.back();
        auto pre = operatorStack.back().precedence - token.precedence;

        while (!operatorStack.empty() && back.expr != "(" && ( pre > 0 || ( pre == 0 && back.associativity == Token::LEFT ) )) {
          outputStack.push_back(back);
          operatorStack.pop_back();
          if (!operatorStack.empty()) {
            back = operatorStack.back();
            pre = operatorStack.back().precedence - token.precedence;
          }
        }
      }
      if (token.expr != "(" && token.expr != ")") operatorStack.push_back(token);
    } else {
      throw std::runtime_error("Unknown token type: " + token.type);
    }
  }
  while (!operatorStack.empty()) {
    auto back = operatorStack.back();
    outputStack.push_back(back);
    operatorStack.pop_back();
  }
#if PRINT2
  std::cout << "outputStack:    ";
  for (auto token : outputStack) {
    std::cout << token.expr << " ";
  }
  std::cout << std::endl;
#endif
  return outputStack;
}

NodePtr Parser::buildTree(const std::vector<Token>& unstacked) {
  std::vector<NodePtr> nodeStack;
  auto tokens = buildStack(unstacked);
  // auto result = evaluateStack(tokens);

  // Safely pop an operand. Throws (instead of dereferencing a garbage
  // shared_ptr via back() on an empty vector) when the expression is
  // malformed and there are more operators than operands. The throw is
  // caught upstream and turned into a graceful "failed to parse" instead
  // of an ESP32 LoadProhibited panic / reboot.
  auto popOperand = [&nodeStack]() -> NodePtr {
    if (nodeStack.empty()) {
      throw std::runtime_error("Malformed expression: operand stack underflow");
    }
    NodePtr n = nodeStack.back();
    nodeStack.pop_back();
    return n;
  };

#if PRINT
  for (size_t i = 0; i < tokens.size(); ++i) {
    std::cout << "token: " << tokens[i].expr << std::endl;
  }
#endif

  for (size_t i = 0; i < tokens.size(); ++i) {
    const auto& token = tokens[i];

#if PRINT
    std::cout << "current token: " << token.expr << std::endl;
#endif

    if (token.type == Token::NUM) {
      auto num = std::make_shared<ConstantNode>(std::stod(token.expr));
      nodeStack.push_back(num);
      continue;
    } else {
      NodePtr operationNode;
      auto var1 = popOperand();

      if (token.expr == "sin") {
        operationNode = std::make_shared<SinNode>(var1);
      } else if (token.expr == "cos") {
        operationNode = std::make_shared<CosNode>(var1);
      } else if (token.expr == "tan") {
        operationNode = std::make_shared<TanNode>(var1);
      } else if (token.expr == "cot") {
        operationNode = std::make_shared<CotNode>(var1);
      } else if (token.expr == "ln") {
        operationNode = std::make_shared<LnNode>(var1);
      } else if (token.expr == "log") {
        operationNode = std::make_shared<LogNode>(var1);
      } else if (token.expr == "sqrt") {
        operationNode = std::make_shared<SqrtNode>(var1);
      } else if (token.expr == "sinh") {
        operationNode = std::make_shared<SinhNode>(var1);
      } else if (token.expr == "cosh") {
        operationNode = std::make_shared<CoshNode>(var1);
      } else if (token.expr == "tanh") {
        operationNode = std::make_shared<TanhNode>(var1);
      } else if (token.expr == "coth") {
        operationNode = std::make_shared<CothNode>(var1);
      } else if (token.expr == "sech") {
        operationNode = std::make_shared<SechNode>(var1);
      } else if (token.expr == "csch") {
        operationNode = std::make_shared<CschNode>(var1);
      } else if (token.expr == "signed") {
        operationNode = std::make_shared<SignedNode>(var1);
      } else if (token.expr == "not") {
        operationNode = std::make_shared<NotNode>(var1);
      } else {
        auto var2 = popOperand();

        if (token.type == Token::OP) {
          if (token.expr == "+") {
            operationNode = std::make_shared<AdditionNode>(var2, var1);
          } else if (token.expr == "-") {
            operationNode = std::make_shared<SubtractionNode>(var2, var1);
          } else if (token.expr == "*") {
            operationNode = std::make_shared<MultiplicationNode>(var2, var1);
          } else if (token.expr == "/") {
            operationNode = std::make_shared<DivisionNode>(var2, var1);
          } else if (token.expr == "^") {
            operationNode = std::make_shared<PowerNode>(var2, var1);
          } else if (token.expr == "<<" || token.expr == "<") {
            operationNode = std::make_shared<BitShiftLeftNode>(var2, var1);
          } else if (token.expr == ">>" || token.expr == ">") {
            operationNode = std::make_shared<BitShiftRightNode>(var2, var1);
          } else if (token.expr == "&&") {
            operationNode = std::make_shared<AndNode>(var2, var1);
          } else if (token.expr == "||") {
            operationNode = std::make_shared<OrNode>(var2, var1);
          } else if (token.expr == "==") {
            operationNode = std::make_shared<EqualNode>(var2, var1);
          }
        } else if (token.type == Token::FUNC) {
          if (token.expr == "bit") {
            operationNode = std::make_shared<BitcompareNode>(var2, var1);
          } else if (token.expr == "int16") {
            operationNode = std::make_shared<ByteConverter16Node>(var2, var1);
          } else {
            auto var3 = popOperand();
            if (token.expr == "int24") {
              operationNode = std::make_shared<ByteConverter24Node>(var3, var2, var1);
            } else {
              auto var4 = popOperand();
              if (token.expr == "int32") {
                operationNode = std::make_shared<ByteConverter32Node>(var4, var3, var2, var1);
              } else if (token.expr == "ifGreaterThan") {
                operationNode = std::make_shared<IfGreaterThanNode>(var4, var3, var2, var1);
              }
            }
          }
        }
      }
      // A token with no node (an operator this parser does not build) would leave a null tree, and readPIDs
      // calls evaluate() on it: refuse the equation instead.
      if (!operationNode) throw std::runtime_error("Unsupported token: " + token.expr);
      nodeStack.push_back(operationNode);
    }
  }
  if (nodeStack.size() != 1) {
    throw std::runtime_error("Incorrect statement: Not enough operators.");
  }

  return nodeStack.back();
}

NodePtr Parser::parse(const std::string& expression) {
  auto v = std::vector<int>{};
  return parse(expression, v);
}

NodePtr Parser::parse(const std::string& expression, const std::vector<int>& variables) {
  return parse(expression, variables, nullptr);
}

NodePtr Parser::parse(const std::string& expression, const std::vector<int>& variables, const ParserValues* values) {
  std::vector<Token> tokens = tokenize(expression, variables, values);
  return buildTree(tokens);
}