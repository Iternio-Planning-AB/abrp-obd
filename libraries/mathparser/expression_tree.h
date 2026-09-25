/**
 * @file expression_tree.h
 * @author Fatih Küçükkarakurt (https://github.com/fkkarakurt)
 *
 * @copyright Copyright (c) 2023
 *
 */

#ifndef EXPRESSION_TREE_H
#define EXPRESSION_TREE_H

#include <memory>
#include <iostream>
#include <cmath>
#include <string>

 // base node class
class Node
{
public:
  virtual ~Node() = default;

  // Calculates the value of this node
  virtual double evaluate() const = 0;
};

using NodePtr = std::shared_ptr<Node>;

class UnaryOperationNode : public Node
{
public:
  explicit UnaryOperationNode(NodePtr operand) : operand_(operand) { }
  virtual ~UnaryOperationNode() = default;

protected:
  NodePtr operand_;
};

class BinaryOperationNode : public Node
{
public:
  explicit BinaryOperationNode(NodePtr left, NodePtr right) : left_(left), right_(right) { }
  virtual ~BinaryOperationNode() = default;

protected:
  NodePtr left_;
  NodePtr right_;
};

// Node representing constant values
class ConstantNode : public Node
{
public:
  ConstantNode(double value);
  double evaluate() const override;

private:
  double value_;
};

// Node performing aggregation between two child nodes
class AdditionNode : public BinaryOperationNode
{
public:
  explicit AdditionNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};

// Node performing subtraction between two child nodes
class SubtractionNode : public BinaryOperationNode
{
public:
  explicit SubtractionNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};

class BitcompareNode : public BinaryOperationNode
{
public:
  explicit BitcompareNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};


// Node performing multiplication between two child nodes
class MultiplicationNode : public BinaryOperationNode
{
public:
  explicit MultiplicationNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};

// Node that performs division between two child nodes
class DivisionNode : public BinaryOperationNode
{
public:
  explicit DivisionNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};

class PowerNode : public BinaryOperationNode
{
public:
  explicit PowerNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};

class BitShiftLeftNode : public BinaryOperationNode
{
public:
  explicit BitShiftLeftNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};

class BitShiftRightNode : public BinaryOperationNode
{
public:
  explicit BitShiftRightNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};

class AndNode : public BinaryOperationNode
{
public:
  explicit AndNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};

class OrNode : public BinaryOperationNode
{
public:
  explicit OrNode(NodePtr left, NodePtr right) : BinaryOperationNode(left, right) { }
  double evaluate() const override;
};

class SinNode : public UnaryOperationNode
{
public:
  explicit SinNode(NodePtr operand) : UnaryOperationNode(operand) { }
  double evaluate() const override;
};

class CosNode : public UnaryOperationNode
{
public:
  explicit CosNode(NodePtr operand) : UnaryOperationNode(operand) { }
  double evaluate() const override;
};

class TanNode : public UnaryOperationNode
{
public:
  explicit TanNode(NodePtr operand) : UnaryOperationNode(operand) { }
  double evaluate() const override;
};

class CotNode : public UnaryOperationNode
{
public:
  explicit CotNode(NodePtr operand) : UnaryOperationNode(operand) { }
  double evaluate() const override;
};

class LnNode : public UnaryOperationNode
{
public:
  explicit LnNode(NodePtr operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class LogNode : public UnaryOperationNode
{
public:
  explicit LogNode(NodePtr operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class SqrtNode : public UnaryOperationNode
{
public:
  explicit SqrtNode(NodePtr operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class SinhNode : public UnaryOperationNode
{
public:
  SinhNode(const NodePtr& operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class CoshNode : public UnaryOperationNode
{
public:
  CoshNode(const NodePtr& operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class TanhNode : public UnaryOperationNode
{
public:
  TanhNode(const NodePtr& operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class CothNode : public UnaryOperationNode
{
public:
  CothNode(const NodePtr& operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class SechNode : public UnaryOperationNode
{
public:
  SechNode(const NodePtr& operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class CschNode : public UnaryOperationNode
{
public:
  CschNode(const NodePtr& operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class SignedNode : public UnaryOperationNode
{
public:
  SignedNode(const NodePtr& operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class NotNode : public UnaryOperationNode
{
public:
  NotNode(const NodePtr& operand) : UnaryOperationNode(operand) { }

  double evaluate() const override;
};

class ByteConverter16Node : public Node
{
public:
  ByteConverter16Node(const NodePtr& var2, const NodePtr& var1);
  double evaluate() const override;

private:
  NodePtr var2_;
  NodePtr var1_;
};

class ByteConverter24Node : public Node
{
public:
  ByteConverter24Node(const NodePtr& var3, const NodePtr& var2, const NodePtr& var1);
  double evaluate() const override;

private:
  NodePtr var3_;
  NodePtr var2_;
  NodePtr var1_;
};

class ByteConverter32Node : public Node
{
public:
  ByteConverter32Node(const NodePtr& var4, const NodePtr& var3, const NodePtr& var2, const NodePtr& var1);
  double evaluate() const override;

private:
  NodePtr var4_;
  NodePtr var3_;
  NodePtr var2_;
  NodePtr var1_;
};

class IfGreaterThanNode : public Node
{
public:
  IfGreaterThanNode(const NodePtr& var4, const NodePtr& var3, const NodePtr& var2, const NodePtr& var1);
  double evaluate() const override;

private:
  NodePtr var4_;
  NodePtr var3_;
  NodePtr var2_;
  NodePtr var1_;
};

#endif // EXPRESSION_TREE_H
