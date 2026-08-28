import parser as ast
from compiler import (
    BinOp,
    Call,
    FuncRet,
    FuncStart,
    Jump,
    JumpIfZero,
    Label,
    Load,
    Ret,
    Store,
    StoreArg,
    Temp,
    Value,
)


class AstToIr:
    def __init__(self, ast_root):
        self.root = ast_root
        self.ir = []
        self.stack_size = 0
        self.temps = {}
        self.labels = {}
        self.current_label = 0
        self.current_temp = 0
        self.curr_loop_end_label = None
        self.in_function = False
        self.var_offsets = {}
        self.locals = {}
        self.current_func_start = None

    def emit(self, ir):
        self.ir.append(ir)

    def alloc(self, temp, size: int):
        self.stack_size += size
        self.temps[temp] = self.stack_size

    def new_temp(self) -> Temp:
        t = Temp(f"t{self.current_temp}")
        self.current_temp += 1
        return t

    def new_label(self) -> Label:
        l = Label(f"L{self.current_label}")
        self.current_label += 1
        return l

    def lower_stmt(self, stmt):
        if isinstance(stmt, ast.If):
            self.lower_if(stmt)
        elif isinstance(stmt, ast.While):
            self.lower_while(stmt)
        elif isinstance(stmt, ast.Assign):
            self.lower_assign(stmt)
        elif isinstance(stmt, ast.FuncCall):
            self.lower_func_call(stmt)
        elif isinstance(stmt, ast.Break) and self.curr_loop_end_label is not None:
            self.emit(Jump(self.curr_loop_end_label))
        elif isinstance(stmt, ast.Return):
            self.lower_return(stmt)
        else:
            raise ValueError(f"Unknown statement type: {type(stmt)}")

    def lower_return(self, stmt):
        value = self.lower_expr(stmt.value)
        if self.in_function:
            self.emit(FuncRet(value))
        else:
            self.emit(Ret(value))

    def lower_func_call(self, stmt: ast.FuncCall):
        args = [self.lower_expr(arg) for arg in stmt.args]
        dst = self.new_temp()
        self.emit(Call(stmt.name, args, dst))
        return dst

    def lower_if(self, stmt):
        cond = self.lower_expr(stmt.cond)
        end_label = self.new_label()
        false_label = self.new_label()
        self.emit(JumpIfZero(cond, false_label))
        for node in stmt.body:
            self.lower_stmt(node)
        self.emit(Jump(end_label))
        self.emit(false_label)
        if stmt.else_body:
            for node in stmt.else_body:
                self.lower_stmt(node)
        self.emit(end_label)

    def lower_while(self, stmt):
        start_label = self.new_label()
        end_label = self.new_label()
        old_loop_end_label = self.curr_loop_end_label
        self.curr_loop_end_label = end_label

        self.emit(start_label)
        cond = self.lower_expr(stmt.cond)
        self.emit(JumpIfZero(cond, end_label))

        for node in stmt.body:
            self.lower_stmt(node)

        self.emit(Jump(start_label))
        self.emit(end_label)
        self.curr_loop_end_label = old_loop_end_label

    def lower_assign(self, stmt: ast.Assign):
        value = self.lower_expr(stmt.value)

        if stmt.name not in self.locals:
            self.alloc(value, 8)
            self.locals[stmt.name] = stmt.name
            self.var_offsets[stmt.name] = self.stack_size
            offset = self.stack_size
        else:
            offset = self.var_offsets[stmt.name]

        self.emit(Store(value, offset))

    def lower_expr(self, expr) -> Value:
        if isinstance(expr, ast.BinOp):
            t1 = self.lower_expr(expr.left)
            t2 = self.lower_expr(expr.right)
            dst = self.new_temp()
            self.emit(BinOp(t1, t2, dst, expr.op))
            return dst
        if isinstance(expr, ast.Name):
            new_temp = self.new_temp()
            self.emit(Load(new_temp, self.var_offsets[expr.id]))
            return new_temp
        if isinstance(expr, ast.Int):
            return expr.value
        if isinstance(expr, ast.FuncCall):
            return self.lower_func_call(expr)

        raise ValueError(f"Unknown expression type: {type(expr)}")

    def compile_func(self, func: ast.Func):
        self.func_reset()
        self.in_function = True

        func_start = FuncStart(func.name, 0)
        self.current_func_start = func_start
        self.emit(func_start)
        for index, arg in enumerate(func.args):
            self.alloc(arg.name, 8)
            self.var_offsets[arg.name] = self.stack_size
            self.locals[arg.name] = arg.name
            self.emit(StoreArg(index, self.stack_size))
        for node in func.body:
            self.lower_stmt(node)

        func_start.stack_size = (self.stack_size + 15) & ~15
        self.current_func_start = None
        self.in_function = False

    def func_reset(self):
        self.stack_size = 0
        self.temps.clear()
        self.var_offsets.clear()
        self.locals.clear()

    def compile(self):
        functions = []
        for node in self.root:
            if isinstance(node, ast.Func):
                functions.append(node)
            else:
                self.lower_stmt(node)

        for func in functions:
            self.compile_func(func)
