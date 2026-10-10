"""Seed step: create the test accounts officers sign in with.

    python -m wadiwatch_backend.seed

Passwords come from a local file that git ignores, by default
backend/data/officers.local.json: one JSON object, account name to password.

- If the file does not exist, it is written with two test accounts and a
  freshly generated password for each.
- If it exists, the accounts in it are created, and an account whose password
  was edited in the file gets the new password (its sessions end).

The passwords are never printed: open the file to read them. Run the step
again after deleting the database. It can run while the backend is running.

Options, each with an environment variable as its default:

  --db PATH            SQLite file (WADIWATCH_DB, default backend/data/wadiwatch.db)
  --credentials PATH   the local file (WADIWATCH_OFFICERS)
"""

import argparse
import json
import os
import secrets
import sys
import time
from pathlib import Path

from .__main__ import DEFAULT_DB
from .officers import NAME_PATTERN, Officers

DEFAULT_CREDENTIALS = DEFAULT_DB.parent / "officers.local.json"
# Account names appear in the alert log. These are roles, not people.
TEST_ACCOUNTS = ("officer1", "officer2")


class SeedError(ValueError):
    """The local credentials file cannot be used. The message never repeats a password."""


def create_accounts(db_path, passwords, clock=time.time):
    """Create or update one account per name in `passwords` (name -> password)."""
    for name, password in passwords.items():
        problem = Officers.problem_with(name, password)
        if problem:
            # A name that is not a valid account name is not repeated either:
            # it may be a password typed in the wrong place.
            valid_name = isinstance(name, str) and NAME_PATTERN.fullmatch(name)
            raise SeedError(f"account {name if valid_name else '(invalid name)'}: {problem}")
    officers = Officers(db_path, clock)
    try:
        for name, password in passwords.items():
            officers.add(name, password)
    finally:
        officers.close()
    return tuple(passwords)


def seed(db_path, credentials_path, clock=time.time):
    """Run the seed step and return the names of the accounts now ready."""
    credentials_path = Path(credentials_path)
    if credentials_path.exists():
        passwords = _read(credentials_path)
    else:
        passwords = {name: secrets.token_urlsafe(18) for name in TEST_ACCOUNTS}
        _write(credentials_path, passwords)
    return create_accounts(db_path, passwords, clock)


def _read(path):
    try:
        with open(path, encoding="utf-8") as credentials:
            passwords = json.load(credentials)
    except (OSError, ValueError) as error:
        raise SeedError(f"{path} could not be read as JSON ({type(error).__name__})") from None
    if not isinstance(passwords, dict) or not passwords:
        raise SeedError(f"{path} must be a JSON object of account name to password")
    return passwords


def _write(path, passwords):
    path.parent.mkdir(parents=True, exist_ok=True)
    # Readable by the owner only, where the system has such a thing.
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as credentials:
        json.dump(passwords, credentials, indent=2)
        credentials.write("\n")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Create the WadiWatch test accounts officers sign in with."
    )
    parser.add_argument(
        "--db", default=os.environ.get("WADIWATCH_DB", DEFAULT_DB), help="SQLite file"
    )
    parser.add_argument(
        "--credentials",
        default=os.environ.get("WADIWATCH_OFFICERS", DEFAULT_CREDENTIALS),
        help="local file of account names and passwords; written if it does not exist",
    )
    arguments = parser.parse_args(argv)

    try:
        names = seed(arguments.db, arguments.credentials)
    except SeedError as error:
        print(f"Seed failed: {error}", file=sys.stderr)
        return 1
    print(f"Accounts ready: {', '.join(names)}")
    print(f"Their passwords are in {arguments.credentials}")
    print("That file is for this machine only. Do not commit it or copy it into a message.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
