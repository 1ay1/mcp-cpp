import os
from .foo import Foo

TODO = "wire it up"  # TODO(alice): finish the wiring
DEBOUNCE = 250


class Widget:
    def __init__(self, name):
        self.name = name        # TODO(bob): validate name

    def render(self):
        return f"<{self.name}>"


def build():
    return Widget("root")
