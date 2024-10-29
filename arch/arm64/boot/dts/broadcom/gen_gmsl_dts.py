#!/usr/bin/env python3

from argparse import ArgumentParser
import json
import re
import sys
from os import path
from typing import TextIO

configs = sys.argv[1:]

vars_type = dict[str, str | int]

def replace_vars(data: str, vars: vars_type) -> str:
    for key, value in vars.items():
        data = re.sub(rf'\${{{key}}}', str(value), data)
    return data

def read_template(dir: str, name: str, vars: vars_type) -> str:
    template_name = f'{name}.dtsi.in'
    template_path = path.join(dir, template_name)
    with open(template_path, 'r') as f:
        data = f.read()

    data = replace_vars(data, vars)

    leftover_replacements = re.findall(r'\${\S+}', data)
    assert not leftover_replacements, \
        f'Leftover varible replacements {leftover_replacements}'

    return data

def write_cam(cam_cfg: any, idx: int, vars: vars_type, config_dir: str, out: TextIO):
    vars = {
        **vars,
        'cam_idx': f'{idx:x}',
    }

    return read_template(config_dir, cam_cfg['name'], vars)


def configure_ser(ser_cfg: any, idx: int, vars: vars_type, config_dir: str, out: TextIO):
    vars = {
        **vars,
        'ser_idx': f'{idx:x}',
    }

    cameras_data = ''
    for i, cam_cfg in enumerate(ser_cfg['cameras']):
        cameras_data += write_cam(cam_cfg, i, vars, config_dir, out)

    vars['cameras'] = cameras_data

    return read_template(config_dir, ser_cfg['name'], vars)

def configure_deser(des_cfg: any, idx: int, config_dir: str, out: TextIO):
    vars = {
        'idx': f'{idx:x}',
    }

    serializers_data = ''
    for i, ser_cfg in enumerate(des_cfg['links']):
        serializers_data += configure_ser(ser_cfg, i, vars, config_dir, out)

    vars['serializers'] = serializers_data

    return read_template(config_dir, des_cfg['name'], vars)


def write_config(config_path: str, dts_path: str | None = None):
    config_dir = path.dirname(config_path)

    config_name = path.basename(config_path)
    config_root, _ = path.splitext(config_name)

    if dts_path is None:
        dts_name = f'{config_root}.dtsi'
        dts_path = path.join(config_dir, dts_name)

    with open(config_path, 'r') as f:
        config = json.load(f)

    with open(dts_path, 'w') as f:
        data = ''
        for i, des_cfg in enumerate(config):
            data += configure_deser(des_cfg, i, config_dir, f)
        f.write(data)

if __name__ == '__main__':
    parser = ArgumentParser(description='Generate GMSL DTS')
    parser.add_argument('-o', '--output', help='DTS output path')
    parser.add_argument('config', help='JSON configuration file')

    args = parser.parse_args()

    write_config(args.config, args.output)
