#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
from release_bundle import ROOT, verify_bundle

def main():
    parser = argparse.ArgumentParser(description='Authenticate and unpack version 0.01 without contacting a tablet.')
    parser.add_argument('destination', type=Path, help='New empty directory to create')
    parser.add_argument('--archive', type=Path, default=ROOT/'release/installed-0.01.tar.gz')
    args = parser.parse_args()
    result = verify_bundle(args.archive, args.destination)
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
