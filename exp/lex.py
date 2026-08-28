from common import ParseException

INDENT_SPACES = 2
keywords = ["if", "else", "while", "break", "return", "fn", "print"]
operators = ["==", "!=", ">=", "<=", ">>","<<", "->", "+", "-", "*", "/", "<", ">", "=", "(", ")", ":", ","]


class Token(str):
    value: str | None = None

    def __new__(cls, name: str, val=None):
        obj = super().__new__(cls, name)
        obj.value = val
        return obj

    def __repr__(self):
        if self.value:
            return f"{str(self)}:{self.value}"
        return str(self)


def lex(text: str):
    total_len = len(text)
    text += "\n\n"
    tokens = []
    i = 0

    c = text[0]
    cc = text[0:2]

    curr_indent = 0

    def add(type, value=None):
        tokens.append(Token(type, value))

    def move(step=1):
        nonlocal i, c, cc
        i += step
        c = text[i]
        cc = text[i : i + 2]

    while i < total_len:
        while not c:
            move()

        if c == "\n":
            move()
            spaces = 0
            while c == " ":
                move()
                spaces += 1

            if c == "\n":
                continue

            add("NEWLINE")
            new_indent = spaces // INDENT_SPACES
            if new_indent - curr_indent > 0:
                add("INDENT")
                curr_indent += 1
            elif new_indent - curr_indent < 0:
                while new_indent - curr_indent < 0:
                    add("DEDENT")
                    curr_indent -= 1

            elif spaces == 1:
                raise ParseException(f"Expected 2 spaces indent, got 1 space")
            continue

        if c == " ":
            move()
            continue

        if cc in operators:
            add(cc)
            move(2)
            continue
        if c in operators:
            add(c)
            move()
            continue

        if c.isdigit():
            start = i
            while c.isdigit():
                move()
            number = text[start:i]
            add("INT", int(number))
            continue
        elif c.isalpha():
            start = i
            while c.isalnum() or c == "_":
                move()
            word = text[start:i]
            if word in keywords:
                add(word)
            else:
                add("NAME", word)
            continue
        elif c == '"':
            start = i
            move()
            while c != '"':
                move()
            add("STRING", text[start:i])
            continue

        move()

    if tokens[-1] != "NEWLINE":
        add("NEWLINE")
    while curr_indent > 0:
        add("DEDENT")
        curr_indent -= 1
    add("EOF")

    return tokens


if __name__ == "__main__":
    text = """
a = 1

fn a():
  return 1
    """
    tokens = lex(text)
    for token in tokens:
        print(token)
