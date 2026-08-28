from arm_backend import ArmBackend
from ast_to_ir import AstToIr
from lex import lex
from parser import Parser

if __name__ == "__main__":
    code = """
fn add(a: int, b: int) -> int
  return a + b

return add(add(2, 3), add(3, 4))
"""
    tokens = lex(code)
    parser = Parser(tokens)
    ast_root = parser.parse()

    compiler = AstToIr(ast_root)
    compiler.compile()
    print(compiler.ir)
    print(ArmBackend(compiler.ir).run())
