import ctypes
import mmap
import struct
from dataclasses import dataclass

from compiler import (
    BinOp,
    Call,
    FuncRet,
    FuncStart,
    IR,
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

COND_INVERT = {
    "==": 1,
    "!=": 0,
    ">=": 11,
    "<": 10,
    ">": 13,
    "<=": 12,
}


@dataclass
class BranchPatch:
    offset: int
    label: str
    kind: str


def arm64(instruction: int) -> bytes:
    return struct.pack("<I", instruction & 0xFFFFFFFF)


class ArmBackend:
    def __init__(self, program: list[IR]):
        self.program = program
        self.code = bytearray()
        self.labels = {}
        self.patches = []
        self.temp_regs = {}
        self.next_reg = 20
        self.current_stack_size = self.compute_stack_size()
        self.in_function = False
        self.stack_size = self.compute_stack_size()

    def compute_stack_size(self):
        size = 0
        for node in self.program:
            if isinstance(node, (Load, Store, StoreArg)):
                size = max(size, node.addr)
        return (size + 15) & ~15

    def emit(self, instruction: int):
        self.code += arm64(instruction)

    def temp_reg(self, temp: Temp):
        if temp.name not in self.temp_regs:
            if self.next_reg > 27:
                raise ValueError("out of temporary registers")
            self.temp_regs[temp.name] = self.next_reg
            self.next_reg += 1
        return self.temp_regs[temp.name]

    def reset_temps(self):
        self.temp_regs = {}
        self.next_reg = 20

    def value_reg(self, value: Value, scratch: int):
        if isinstance(value, int):
            self.mov_imm(scratch, value)
            return scratch
        return self.temp_reg(value)

    def mov_imm(self, dst: int, value: int):
        if not 0 <= value <= 0xFFFF:
            raise ValueError("integer literals are currently limited to 16 bits")
        self.emit(0xD2800000 | (value << 5) | dst)

    def mov_reg(self, dst: int, src: int):
        if dst != src:
            self.emit(0xAA0003E0 | (src << 16) | dst)

    def add(self, dst: int, left: int, right: int):
        self.emit(0x8B000000 | (right << 16) | (left << 5) | dst)

    def sub(self, dst: int, left: int, right: int):
        self.emit(0xCB000000 | (right << 16) | (left << 5) | dst)

    def mul(self, dst: int, left: int, right: int):
        self.emit(0x9B007C00 | (right << 16) | (left << 5) | dst)

    def div(self, dst: int, left: int, right: int):
        self.emit(0x9AC00C00 | (right << 16) | (left << 5) | dst)

    def cmp(self, left: int, right: int):
        self.emit(0xEB00001F | (right << 16) | (left << 5))

    def cset(self, dst: int, op: str):
        if op not in COND_INVERT:
            raise ValueError(f"unsupported comparison operator: {op}")
        self.emit(0x9A9F07E0 | (COND_INVERT[op] << 12) | dst)

    def branch(self, label: Label):
        self.patches.append(BranchPatch(len(self.code), label.name, "b"))
        self.emit(0x14000000)

    def call(self, name: str):
        self.patches.append(BranchPatch(len(self.code), name, "bl"))
        self.emit(0x94000000)

    def cbz(self, reg: int, label: Label):
        self.patches.append(BranchPatch(len(self.code), label.name, "cbz"))
        self.emit(0xB4000000 | reg)

    def add_sp(self, amount: int):
        if amount:
            self.emit(0x91000000 | (amount << 10) | (31 << 5) | 31)

    def sub_sp(self, amount: int):
        if amount:
            self.emit(0xD1000000 | (amount << 10) | (31 << 5) | 31)

    def stp_fp_lr_pre(self):
        self.stp_pre(29, 30)

    def ldp_fp_lr_post(self):
        self.ldp_post(29, 30)

    def stp_pre(self, rt: int, rt2: int):
        self.emit(0xA9800000 | ((-2 & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt)

    def ldp_post(self, rt: int, rt2: int):
        self.emit(0xA8C00000 | (2 << 15) | (rt2 << 10) | (31 << 5) | rt)

    def save_top_level_registers(self):
        self.stp_pre(19, 20)
        self.stp_pre(21, 22)
        self.stp_pre(23, 24)
        self.stp_pre(25, 26)
        self.stp_pre(27, 28)

    def restore_top_level_registers(self):
        self.ldp_post(27, 28)
        self.ldp_post(25, 26)
        self.ldp_post(23, 24)
        self.ldp_post(21, 22)
        self.ldp_post(19, 20)

    def save_function_registers(self):
        self.stp_pre(20, 21)
        self.stp_pre(22, 23)
        self.stp_pre(24, 25)
        self.stp_pre(26, 27)

    def restore_function_registers(self):
        self.ldp_post(26, 27)
        self.ldp_post(24, 25)
        self.ldp_post(22, 23)
        self.ldp_post(20, 21)

    def mov_fp_sp(self):
        self.emit(0x910003FD)

    def ldr(self, dst: int, offset: int):
        self.emit(0xF9400000 | (((offset - 8) // 8) << 10) | (31 << 5) | dst)

    def str(self, src: int, offset: int):
        self.emit(0xF9000000 | (((offset - 8) // 8) << 10) | (31 << 5) | src)

    def ret(self):
        self.emit(0xD65F03C0)

    def patch_branches(self):
        for patch in self.patches:
            if patch.label not in self.labels:
                raise ValueError(f"unknown label: {patch.label}")
            delta = (self.labels[patch.label] - patch.offset) // 4
            if patch.kind == "b":
                instruction = 0x14000000 | (delta & 0x03FFFFFF)
            elif patch.kind == "bl":
                instruction = 0x94000000 | (delta & 0x03FFFFFF)
            elif patch.kind == "cbz":
                reg = self.code[patch.offset] & 0x1F
                instruction = 0xB4000000 | ((delta & 0x7FFFF) << 5) | reg
            else:
                raise ValueError(f"unknown branch patch kind: {patch.kind}")
            self.code[patch.offset : patch.offset + 4] = arm64(instruction)

    def compile(self):
        self.save_top_level_registers()
        self.mov_reg(19, 30)
        self.sub_sp(self.stack_size)

        for node in self.program:
            if isinstance(node, FuncStart):
                self.in_function = True
                self.current_stack_size = node.stack_size
                self.reset_temps()
                self.labels[node.name] = len(self.code)
                self.stp_fp_lr_pre()
                self.mov_fp_sp()
                self.save_function_registers()
                self.sub_sp(self.current_stack_size)
            elif isinstance(node, Label):
                self.labels[node.name] = len(self.code)
            elif isinstance(node, BinOp):
                left = self.value_reg(node.t1, 16)
                right = self.value_reg(node.t2, 17)
                dst = self.temp_reg(node.dst)
                if node.op == "+":
                    self.add(dst, left, right)
                elif node.op == "-":
                    self.sub(dst, left, right)
                elif node.op == "*":
                    self.mul(dst, left, right)
                elif node.op == "/":
                    self.div(dst, left, right)
                elif node.op in COND_INVERT:
                    self.cmp(left, right)
                    self.cset(dst, node.op)
                else:
                    raise ValueError(f"unsupported binary operator: {node.op}")
            elif isinstance(node, Jump):
                self.branch(node.label)
            elif isinstance(node, JumpIfZero):
                self.cbz(self.temp_reg(node.t1), node.label)
            elif isinstance(node, Call):
                if len(node.args) > 8:
                    raise ValueError("function calls support up to 8 arguments")
                for index, arg in enumerate(node.args):
                    self.mov_reg(index, self.value_reg(arg, 16))
                self.call(node.name)
                self.mov_reg(self.temp_reg(node.dst), 0)
            elif isinstance(node, Load):
                self.ldr(self.temp_reg(node.t1), node.addr)
            elif isinstance(node, Store):
                self.str(self.value_reg(node.t1, 16), node.addr)
            elif isinstance(node, StoreArg):
                if node.index > 7:
                    raise ValueError("function definitions support up to 8 arguments")
                self.str(node.index, node.addr)
            elif isinstance(node, Ret):
                self.mov_reg(0, self.value_reg(node.value, 16))
                self.add_sp(self.current_stack_size)
                self.mov_reg(30, 19)
                self.restore_top_level_registers()
                self.ret()
            elif isinstance(node, FuncRet):
                self.mov_reg(0, self.value_reg(node.value, 16))
                self.add_sp(self.current_stack_size)
                self.restore_function_registers()
                self.ldp_fp_lr_post()
                self.ret()
            else:
                raise ValueError(f"unsupported IR node: {node}")

        self.mov_imm(0, 0)
        self.add_sp(self.stack_size)
        self.mov_reg(30, 19)
        self.restore_top_level_registers()
        self.ret()
        self.patch_branches()
        return bytes(self.code)

    def run(self):
        code = self.compile()
        libc = ctypes.CDLL(None)
        libc.mmap.restype = ctypes.c_void_p

        address = libc.mmap(
            None,
            mmap.PAGESIZE,
            mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC,
            mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS | 0x800,  # MAP_JIT
            -1,
            0,
        )
        if address == ctypes.c_void_p(-1).value:
            raise OSError("mmap failed")

        try:
            protect = libc.pthread_jit_write_protect_np
            protect.argtypes = [ctypes.c_int]
            protect(0)
        except AttributeError:
            protect = None

        ctypes.memmove(address, code, len(code))
        if protect:
            protect(1)

        fn = ctypes.CFUNCTYPE(ctypes.c_long)(address)
        return fn()
