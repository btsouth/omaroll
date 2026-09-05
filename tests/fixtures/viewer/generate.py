#!/usr/bin/env python3
"""Regenerate original viewer test media. Requires Pillow and FFmpeg."""
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parent
frames = []
for index in range(12):
    frame = Image.new('RGBA', (160, 100), (0, 0, 0, 0))
    draw = ImageDraw.Draw(frame)
    draw.rectangle((index * 10, 30, index * 10 + 35, 65), fill=(240, 80, 40, 255))
    draw.text((8, 8), f'Frame {index + 1:02}', fill=(20, 160, 240, 255))
    frames.append(frame)
for suffix in ('gif', 'webp'):
    frames[0].save(root / f'animated.{suffix}', save_all=True,
                   append_images=frames[1:], duration=120, loop=0,
                   disposal=2, lossless=True)
frames[0].save(root / 'still.webp', lossless=True)
frames[0].save(root / 'transparent.png')
frames[0].save(root / 'truecolor.tga')
subprocess.run([
    'ffmpeg', '-hide_banner', '-loglevel', 'error',
    '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=12:duration=12',
    '-f', 'lavfi', '-i', 'sine=frequency=440:duration=12',
    '-f', 'lavfi', '-i', 'sine=frequency=880:duration=12',
    '-i', str(root / 'captions.srt'),
    '-map', '0:v', '-map', '1:a', '-map', '2:a', '-map', '3:s',
    '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '32',
    '-c:a', 'aac', '-b:a', '32k', '-c:s', 'srt',
    '-metadata:s:a:0', 'language=eng', '-metadata:s:a:1', 'language=fra',
    '-metadata:s:s:0', 'language=eng', '-y', str(root / 'tracks.mkv'),
], check=True)
