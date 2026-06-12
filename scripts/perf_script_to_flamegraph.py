#!/usr/bin/env python3

import argparse
import html
import re
from pathlib import Path


FRAME_RE = re.compile(r"^\s+[0-9a-fA-F]+\s+(.+?)(?:\s+\(|$)")


def collapse_perf_script(text):
    stacks = {}
    current = []

    def flush():
        if not current:
            return
        stack = ";".join(reversed(current))
        stacks[stack] = stacks.get(stack, 0) + 1
        current.clear()

    for line in text.splitlines():
        if not line.strip():
            flush()
            continue
        match = FRAME_RE.match(line)
        if match:
            frame = match.group(1).strip()
            if frame and frame != "[unknown]":
                current.append(frame)
    flush()
    return stacks


def build_tree(stacks):
    root = {"name": "all", "value": 0, "children": {}}
    for stack, count in stacks.items():
        root["value"] += count
        node = root
        for frame in stack.split(";"):
            children = node["children"]
            if frame not in children:
                children[frame] = {"name": frame, "value": 0, "children": {}}
            node = children[frame]
            node["value"] += count
    return root


def color_for(name):
    seed = sum(ord(ch) for ch in name)
    hue = 20 + seed % 40
    sat = 55 + seed % 25
    light = 58 + seed % 15
    return f"hsl({hue},{sat}%,{light}%)"


def layout(node, x, y, width, frame_height, frames):
    children = sorted(node["children"].values(), key=lambda item: item["value"], reverse=True)
    cursor = x
    for child in children:
        if node["value"] == 0:
            child_width = 0
        else:
            child_width = width * child["value"] / node["value"]
        if child_width >= 0.5:
            frames.append((cursor, y, child_width, frame_height, child["name"], child["value"]))
            layout(child, cursor, y + frame_height, child_width, frame_height, frames)
        cursor += child_width


def render_svg(stacks, width):
    tree = build_tree(stacks)
    frames = []
    frame_height = 18
    layout(tree, 0, 0, width, frame_height, frames)
    height = max((y for _, y, _, _, _, _ in frames), default=0) + frame_height + 28
    total = tree["value"] or 1
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        "<style>text{font-family:monospace;font-size:12px} .frame:hover{stroke:#000;stroke-width:1}</style>",
        f'<text x="8" y="16">Samples: {total}</text>',
    ]
    for x, y, w, h, name, value in frames:
        y += 22
        title = html.escape(f"{name} ({value} samples, {value * 100.0 / total:.2f}%)")
        label = html.escape(name)
        parts.append(f"<g><title>{title}</title>")
        parts.append(
            f'<rect class="frame" x="{x:.2f}" y="{y:.2f}" width="{max(w - 0.5, 0):.2f}" '
            f'height="{h - 1}" fill="{color_for(name)}"/>'
        )
        if w > 40:
            max_chars = max(1, int(w / 7))
            text = label if len(label) <= max_chars else label[: max_chars - 1] + "."
            parts.append(f'<text x="{x + 3:.2f}" y="{y + 13:.2f}">{text}</text>')
        parts.append("</g>")
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Convert perf script output to a simple SVG flamegraph")
    parser.add_argument("perf_script")
    parser.add_argument("--folded", required=True)
    parser.add_argument("--svg", required=True)
    parser.add_argument("--width", type=int, default=1400)
    args = parser.parse_args()

    text = Path(args.perf_script).read_text(encoding="utf-8", errors="replace")
    stacks = collapse_perf_script(text)
    folded = "\n".join(f"{stack} {count}" for stack, count in sorted(stacks.items())) + "\n"
    Path(args.folded).write_text(folded, encoding="utf-8")
    Path(args.svg).write_text(render_svg(stacks, args.width), encoding="utf-8")


if __name__ == "__main__":
    main()
