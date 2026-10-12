/**
 * @file expression_tree.cpp
 * @author Fatih Küçükkarakurt (https://github.com/fkkarakurt)
 *
 * @copyright Copyright (c) 2023
 *
 */

#include "expression_tree.h"
#include <stdexcept>

 // ConstantNode implementation
ConstantNode::ConstantNode(double value) : value_(value) { }
double ConstantNode::evaluate() const {
  return value_;
}

// AdditionNode implementation
double AdditionNode::evaluate() const {
  // std::cout << "Evaluating op: + " << std::endl;
  auto left = left_->evaluate();
  auto right = right_->evaluate();
  auto res = left_->evaluate() + right_->evaluate();
  return res;
}

// SubtractionNode implementation
double SubtractionNode::evaluate() const {
  auto left = left_->evaluate();
  auto right = right_->evaluate();
  // std::cout << "Evaluating op: - . left: " << left << ", right: " << right << std::endl;
  return left - right;
}

// BitcompareNode implementation
double BitcompareNode::evaluate() const {
  auto integer = static_cast<int>(left_->evaluate());
  auto bit = static_cast<int>(right_->evaluate());
  auto comp = integer >> (bit);
  // std::cout << "Evaluating BIT: " << integer << " : " << bit << " : " << comp << std::endl;

  return comp & 1;
  // auto res = integer | bit;
  // return res == bit; //integer OR bit is same as right if the bit is set in given int  
}

// MultiplicationNode implementation
double MultiplicationNode::evaluate() const {
  auto left = left_->evaluate();
  auto right = right_->evaluate();
  auto res = left_->evaluate() * right_->evaluate();
  return res;
}

// DivisionNode implementation
double DivisionNode::evaluate() const {
  auto left = left_->evaluate();
  double denominator = right_->evaluate();
  if (denominator == 0.0) {
    // Throw, never exit(): exit() reboots the ESP32 and the denominator is often a reply byte.
    throw std::runtime_error("Division by 0");
  }
  return left_->evaluate() / denominator;
}

double PowerNode::evaluate() const {
  return std::pow(left_->evaluate(), right_->evaluate());
}

double BitShiftLeftNode::evaluate() const {
  return static_cast<int>( left_->evaluate() ) << static_cast<int>( right_->evaluate() );
}

double BitShiftRightNode::evaluate() const {
  return static_cast<int>( left_->evaluate() ) >> static_cast<int>( right_->evaluate() );
}

double AndNode::evaluate() const {
  return left_->evaluate() && right_->evaluate();
}

double OrNode::evaluate() const {
  return left_->evaluate() || right_->evaluate();
}

double SinNode::evaluate() const {
  return std::sin(operand_->evaluate());
}

double CosNode::evaluate() const {
  return std::cos(operand_->evaluate());
}

double TanNode::evaluate() const {
  return std::tan(operand_->evaluate());
}

double CotNode::evaluate() const {
  double tanValue = std::tan(operand_->evaluate());
  if (tanValue == 0.0) {
    throw std::runtime_error("Cot of a zero tan");   // throw, not exit() - see DivisionNode
  }
  return 1.0 / tanValue;
}

double LnNode::evaluate() const {
  return std::log(operand_->evaluate());
}

double LogNode::evaluate() const {
  return std::log10(operand_->evaluate());
}

double SqrtNode::evaluate() const {
  double value = operand_->evaluate();
  if (value < 0.0) {
    throw std::runtime_error("Square root of a negative number");   // throw, not exit() - see DivisionNode
  }
  return std::sqrt(value);
}

double SinhNode::evaluate() const

{
  return std::sinh(operand_->evaluate());
}
double CoshNode::evaluate() const {
  return std::cosh(operand_->evaluate());
}

double TanhNode::evaluate() const {
  return std::tanh(operand_->evaluate());
}

double CothNode::evaluate() const {
  double tanhVal = std::tanh(operand_->evaluate());
  if (tanhVal != 0) {
    return 1 / tanhVal;
  }

  // Infinity for division by zero
  return HUGE_VAL;
}

double SechNode::evaluate() const {
  return 1 / std::cosh(operand_->evaluate());
}


double CschNode::evaluate() const {
  double sinhVal = std::sinh(operand_->evaluate());
  if (sinhVal != 0) {
    return 1 / sinhVal;
  }

  // Infinity for division by zero
  return HUGE_VAL;
}

double SignedNode::evaluate() const {
  return static_cast<int8_t>(static_cast<int>(operand_->evaluate()));
}

double NotNode::evaluate() const {
  return !operand_->evaluate();
}

ByteConverter16Node::ByteConverter16Node(const NodePtr& var2, const NodePtr& var1) : var2_{ var2 }, var1_{ var1 } { };
double ByteConverter16Node::evaluate() const {
  auto b1 = var1_->evaluate();
  auto b2 = var2_->evaluate() * 256;
  return b1 + b2;
}

ByteConverter24Node::ByteConverter24Node(const NodePtr& var3, const NodePtr& var2, const NodePtr& var1) : var3_{ var3 }, var2_{ var2 }, var1_{ var1 } { };
double ByteConverter24Node::evaluate() const {
  auto b1 = var1_->evaluate();
  auto b2 = var2_->evaluate() * 256;
  auto b3 = var3_->evaluate() * 65536;
  return b1 + b2 + b3;
}

ByteConverter32Node::ByteConverter32Node(const NodePtr& var4, const NodePtr& var3, const NodePtr& var2, const NodePtr& var1) : var4_{ var4 }, var3_{ var3 }, var2_{ var2 }, var1_{ var1 } { };
double ByteConverter32Node::evaluate() const {
  auto b1 = var1_->evaluate();
  auto b2 = var2_->evaluate() * 256;
  auto b3 = var3_->evaluate() * 65536;
  auto b4 = var4_->evaluate() * 16777216;
  return b1 + b2 + b3 + b4;
}

IfGreaterThanNode::IfGreaterThanNode(const NodePtr& var4, const NodePtr& var3, const NodePtr& var2, const NodePtr& var1) : var4_{ var4 }, var3_{ var3 }, var2_{ var2 }, var1_{ var1 } { };
double IfGreaterThanNode::evaluate() const {
  auto b1 = var1_->evaluate();
  auto b2 = var2_->evaluate();
  auto b3 = var3_->evaluate();
  auto b4 = var4_->evaluate();
  // std::cout << "b1: " << b1 << ", b2: " << b2 << ", b3: " << b3 << ", b4: " << b4 << std::endl;  
  return b4 > b3 ? b2 : b1;
}