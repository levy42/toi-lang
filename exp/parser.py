import struct
from dataclasses import dataclass
from pprint import pp

from common import ParseException
from lex import Token


@dataclass
class Node: ...


@dataclass
class BinOp(Node):
    op: str
    left: Node
    right: Node


@dataclass
class CircOp(BinOp): ...


@dataclass
class Name(Node):
    id: str


@dataclass
class Value(Node):
    pass


@dataclass
class Int(Value):
    value: int


@dataclass
class FuncArg(Node):
    name: str
    type: str | None = None


@dataclass
class Func(Node):
    name: str
    args: list[FuncArg]
    body: list[Node]
    return_kind: str | None = None


@dataclass
class Print(Node):
    value: Node


@dataclass
class FuncCall(Node):
    name: str
    args: list[Node]


@dataclass
class Return(Node):
    value: Node | None = None


@dataclass
class If(Node):
    cond: Node
    body: list[Node]
    else_body: list[Node] | None = None


@dataclass
class While(Node):
    cond: Node
    body: list[Node]


@dataclass
class Assign(Node):
    name: str
    value: Node


@dataclass
class Break(Node):
    pass


INFIX = {
    # comparison
    ">": 10,
    "<": 10,
    "==": 10,
    "!=": 10,
    ">=": 10,
    "<=": 10,
    # math
    "+": 20,
    "-": 20,
    "*": 30,
    "/": 30,
}


class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.i = 0

    def match(self, *tokens):
        for token in tokens:
            if self.tokens[self.i] == token:
                t = self.tokens[self.i]
                self.i += 1
                return t
        return False

    def eat(self, token: str):
        c = self.curr
        if c == token:
            self.i += 1
            if token == "NAME":
                return c.value
            return c
        else:
            raise ParseException(f"Expected {token}, got {self.curr}{self.curr.value}")

    @property
    def curr(self) -> Token:
        return self.tokens[self.i]

    @property
    def peek(self) -> Token:
        return self.tokens[self.i + 1]

    def at(self, token):
        return self.curr == token

    def parse(self):
        body = []
        while not self.at("EOF"):
            if self.match("NEWLINE"):
                continue

            stmt = self.parse_stmt()
            body.append(stmt)

        return body

    def parse_args(self) -> list[Node]:
        args = []
        while not self.match(")"):
            arg = self.parse_expr()
            args.append(arg)
            if self.match(")"):
                break
            self.eat(",")
        return args

    def parse_atom(self) -> Node:
        t = self.curr
        if self.match("NAME"):
            if self.match("("):
                args = self.parse_args()
                return FuncCall(t.value, args)
            return Name(t.value)
        elif self.match("INT"):
            return Int(t.value)
        elif self.match("("):
            expr = self.parse_expr()
            self.eat(")")
            return expr
        else:
            raise ParseException(
                f"Unexpected token {t}:{t.value} at {self.i} ({self.tokens[self.i - 1 : self.i + 1]})"
            )

    def parse_expr(self, min_bp: int = 0) -> Node:
        left = self.parse_atom()
        while True:
            op = self.curr
            if op not in INFIX:
                break
            bp = INFIX[op]
            if bp < min_bp:
                break
            self.eat(op)
            right = self.parse_expr(bp)
            left = BinOp(op, left, right)

        return left

    def parse_simple(self):
        expr = self.parse_expr()
        if self.match("="):
            if type(expr) is not Name:
                raise ParseException("Assignment target must be a variable name")
            value = self.parse_expr()
            return Assign(expr.id, value)

        return expr

    def parse_stmt(self):
        if self.match("if"):
            return self.parse_if()
        elif self.match("else"):
            pass
        elif self.match("while"):
            return self.parse_while()
        elif self.match("fn"):
            return self.parse_func()
        elif self.match("return"):
            return Return(self.parse_expr())
        elif self.match("print"):
            return Print(self.parse_expr())
        elif self.match("break"):
            return Break()
        else:
            return self.parse_simple()

    def parse_if(self):
        cond = self.parse_expr()
        body = self.parse_block()
        else_body = None
        if self.match("elif"):
            else_body = [self.parse_if()]
        elif self.match("else"):
            else_body = self.parse_block()
        return If(cond, body, else_body)

    def parse_while(self):
        cond = self.parse_expr()
        body = self.parse_block()
        return While(cond, body)

    def parse_block(self):
        body = []
        self.eat("NEWLINE")
        self.eat("INDENT")
        while not self.match("DEDENT"):
            body.append(self.parse_stmt())
            self.eat("NEWLINE")
        return body

    def parse_func(self):
        name = self.eat("NAME")
        self.eat("(")
        args = []
        while not self.match(")"):
            arg = FuncArg(self.eat("NAME"))
            self.eat(":")
            arg.type = self.eat("NAME")

            args.append(arg)
            if self.match(","):
                pass

        self.eat("->")
        return_kind = self.eat("NAME")
        body = self.parse_block()
        return Func(name, args, body, return_kind=return_kind)


if __name__ == "__main__":
    from lex import lex

    code = """
fn add(a: int, b: int) -> int
  c = a + b
  return c

print(add(1, 2))
"""
    tokens = lex(code)
    pp(tokens)
    parser = Parser(tokens)
    pp(parser.parse())
