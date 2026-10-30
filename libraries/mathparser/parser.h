/**
 * @file parser.h
 * @author Fatih Küçükkarakurt (https://github.com/fkkarakurt)
 *
 * @copyright Copyright (c) 2023
 *
 * Expanded by Ludvig Delvret for Iternio
 * 
 */

#ifndef PARSER_H
#define PARSER_H

#include "expression_tree.h"
#include <utility>
#include <vector>
#include "../OBDWrapper/obd_vector.h"

// Named values VAL{name} reads: a poll's results so far, the newest last.
using ParserValues = OBDVector;

struct Token {
  enum TYPE {
    NUM,
    FUNC,
    OP
  };

  enum ASSOCIATIVITY {
    LEFT,
    RIGHT
  };

  Token() = delete;
  Token(std::string expr, TYPE type) : expr{ expr }, type{ type } { };
  Token(std::string expr, TYPE type, int pre) : expr{ expr }, type{ type }, precedence{ pre } { };
  Token(std::string expr, TYPE type, int pre, ASSOCIATIVITY ass) : expr{ expr }, type{ type }, precedence{ pre }, associativity{ ass } { };

  std::string expr;
  TYPE type;
  int precedence{ 0 };
  ASSOCIATIVITY associativity{ LEFT };


};

class Parser
{
public:
  // Parses the expression and returns an expression tree
  NodePtr parse(const std::string& expression);

  NodePtr parse(const std::string& expression, const std::vector<int>& variables);

  // As above, with VAL{name} taken from values (the last entry of that name). A name it lacks throws.
  NodePtr parse(const std::string& expression, const std::vector<int>& variables, const ParserValues* values);

private:
  std::vector<Token> tokenize(const std::string& expression, const std::vector<int>& variables,
                              const ParserValues* values);

  //Creates outputStack in RPN according to shunting yard algorithm
  std::vector<Token> buildStack(const std::vector<Token>& tokens);

  NodePtr buildTree(const std::vector<Token>& tokens);
};


#endif // PARSER_H
