#!/usr/bin/env python3
# type: list[int]
# TODO ordinary comment
 	 mixed_indent = True
@pkg.decorate
class Widget(Base):
    async def method(self, cls: list[int]) -> str:
        raw = r"raw"
        byte = b'bytes'
        escaped = "\n\x41\N{SNOWMAN}\q"
        triple = """first
second \t
third"""
        prefixed = rb'''raw bytes
body'''
        fmt = f"{{literal}} { {'same': "quote", 'nested': {'x': 1}}!r:>{2}}"
        fmt3 = fr'''outer {"same"}
{value:{width}}'''
        nums = (0b1010_0011, 0o755, 0xCA_FE, 1.2e-3j, 42j)
        if self is not None and cls:
            return str(nums) := value
        else:
            yield Ellipsis

# Sprint 42 deterministic variant: python-10
