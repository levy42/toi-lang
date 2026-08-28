from dataclasses import dataclass


@dataclass(frozen=True)
class Temp:
    name: str


Value = int | Temp


@dataclass
class IR:
    pass


@dataclass
class BinOp(IR):
    t1: Value
    t2: Value
    dst: Temp
    op: str


@dataclass
class Label(IR):
    name: str


@dataclass
class Jump(IR):
    label: Label


@dataclass
class JumpIfZero(IR):
    t1: Temp
    label: Label


@dataclass
class Load(IR):
    t1: Temp
    addr: int


@dataclass
class Store(IR):
    t1: Value
    addr: int


@dataclass
class MoveStackPointer(IR):
    size: int


@dataclass
class Ret(IR):
    value: Value


@dataclass
class FuncStart(IR):
    name: str
    stack_size: int


@dataclass
class FuncRet(IR):
    value: Value


@dataclass
class MoveToArg(IR):
    value: Value


@dataclass
class StoreArg(IR):
    index: int
    addr: int


@dataclass
class Call(IR):
    name: str
    args: list[Value]
    dst: Temp
