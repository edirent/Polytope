#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import stat
import sys
import time
from pathlib import Path

from dotenv import dotenv_values
from eth_account import Account
from py_clob_client.client import ClobClient
from py_clob_client.constants import POLYGON


TARGET_KEYS = (
    "POLYMARKET_L2_API_KEY",
    "POLYMARKET_L2_SECRET",
    "POLYMARKET_L2_PASSPHRASE",
)


def redacted(value: str) -> str:
    if not value:
        return "<missing>"
    if len(value) <= 8:
        return "<redacted>"
    return f"{value[:4]}...{value[-4:]} len={len(value)}"


def load_config(path: Path) -> dict[str, str]:
    values = dict(dotenv_values(path))
    for key, value in os.environ.items():
        values.setdefault(key, value)
    return {k: v for k, v in values.items() if v is not None}


def replace_export_line(text: str, key: str, value: str) -> str:
    pattern = re.compile(rf'^(export\s+{re.escape(key)}=).*$',
                         flags=re.MULTILINE)
    replacement = rf'\1"{value}"'
    if pattern.search(text):
        return pattern.sub(replacement, text)
    return text.rstrip() + f'\nexport {key}="{value}"\n'


def write_config(path: Path, values: dict[str, str]) -> Path:
    timestamp = time.strftime("%Y%m%d_%H%M%S", time.gmtime())
    backup = path.with_suffix(path.suffix + f".bak.{timestamp}")
    shutil.copy2(path, backup)

    text = path.read_text(encoding="utf-8")
    for key, value in values.items():
        text = replace_export_line(text, key, value)
    path.write_text(text, encoding="utf-8")
    path.chmod(stat.S_IRUSR | stat.S_IWUSR)
    return backup


def mint_creds(client: ClobClient, mode: str):
    if mode == "derive":
        return client.derive_api_key()
    if mode == "create":
        return client.create_api_key()
    if mode == "create-or-derive":
        return client.create_or_derive_api_creds()
    raise ValueError(f"unsupported mode: {mode}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="config/credentials.local.env")
    parser.add_argument("--host", default="https://clob.polymarket.com")
    parser.add_argument(
        "--mode",
        choices=("create-or-derive", "create", "derive"),
        default="create-or-derive",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--print-shell",
        action="store_true",
        help="print full export lines; intentionally off by default",
    )
    args = parser.parse_args()

    config_path = Path(args.config)
    values = load_config(config_path)
    private_key = values.get("POLYMARKET_WALLET_PRIVATE_KEY", "")
    l2_address = values.get("POLYMARKET_L2_ADDRESS", "")
    if not private_key:
        raise RuntimeError("POLYMARKET_WALLET_PRIVATE_KEY is missing")

    derived_address = Account.from_key(private_key).address
    if l2_address and derived_address.lower() != l2_address.lower():
        raise RuntimeError(
            "POLYMARKET_L2_ADDRESS does not match POLYMARKET_WALLET_PRIVATE_KEY"
        )

    print("mint_l2_keys:")
    print(f"  mode: {args.mode}")
    print(f"  host: {args.host}")
    print(f"  signer: {derived_address[:6]}...{derived_address[-4:]}")

    client = ClobClient(args.host, key=private_key, chain_id=POLYGON)
    creds = mint_creds(client, args.mode)
    if creds is None:
        raise RuntimeError("py-clob-client returned no credentials")

    # Keep the secret decodable by both urlsafe and standard base64 decoders.
    safe_secret = creds.api_secret.replace("_", "/").replace("-", "+")
    new_values = {
        "POLYMARKET_L2_API_KEY": creds.api_key,
        "POLYMARKET_L2_SECRET": safe_secret,
        "POLYMARKET_L2_PASSPHRASE": creds.api_passphrase,
    }

    print("  minted:")
    for key in TARGET_KEYS:
        print(f"    {key}: {redacted(new_values[key])}")

    if args.print_shell:
        for key in TARGET_KEYS:
            print(f'export {key}="{new_values[key]}"')

    if args.dry_run:
        print("  config_updated: false")
        return 0

    backup = write_config(config_path, new_values)
    print(f"  config_updated: true")
    print(f"  backup: {backup}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"mint_l2_keys_error: {type(exc).__name__}: {exc}", file=sys.stderr)
        raise SystemExit(1)
