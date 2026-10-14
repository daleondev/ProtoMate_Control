"""Small reader for inspecting KiCad/Specctra documents; no design mutation."""
import json
import re


def parse(text):
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+', text)
    stack = []
    root = None
    for token in tokens:
        if token == '(':
            node = []
            if stack:
                stack[-1].append(node)
            stack.append(node)
        elif token == ')':
            root = stack.pop()
        else:
            stack[-1].append(json.loads(token) if token.startswith('"') else token)
    return root


def children(node, tag):
    return [x for x in node if isinstance(x, list) and x and x[0] == tag]


def child(node, tag, default=None):
    return next(iter(children(node, tag)), default)


def property_value(node, name):
    return next(x[2] for x in children(node, 'property') if x[1] == name)
