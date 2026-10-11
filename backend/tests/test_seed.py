"""The seed step creates the test accounts officers sign in with."""

import json

import pytest
from officer_support import Account, Browser

from wadiwatch_backend import seed
from wadiwatch_backend.officers import MIN_PASSWORD_LENGTH

pytestmark = pytest.mark.usefixtures("quick_password_hashing")

TOO_SHORT = "x7#" * ((MIN_PASSWORD_LENGTH - 1) // 3)


@pytest.fixture
def credentials_file(tmp_path):
    return tmp_path / "local" / "officers.local.json"


def accounts_in(credentials_file):
    stored = json.loads(credentials_file.read_text(encoding="utf-8"))
    return [Account(name, password) for name, password in stored.items()]


def test_seed_creates_test_accounts_and_writes_their_generated_passwords_to_the_local_file(
    harness, visitor, credentials_file
):
    names = seed.seed(harness.db_path, credentials_file)

    accounts = accounts_in(credentials_file)
    assert [account.name for account in accounts] == list(names) == list(seed.TEST_ACCOUNTS)
    assert len(accounts) >= 2
    assert len({account.password for account in accounts}) == len(accounts)
    for account in accounts:
        assert len(account.password) >= 16
        assert visitor.sign_in(account).status_code == 200
        assert visitor.session()["officer"] == account.name


def test_seed_generates_different_passwords_every_time(harness, tmp_path):
    seed.seed(harness.db_path, tmp_path / "one.json")
    seed.seed(harness.db_path, tmp_path / "two.json")

    one = {account.password for account in accounts_in(tmp_path / "one.json")}
    two = {account.password for account in accounts_in(tmp_path / "two.json")}
    assert not one & two


def test_seed_run_again_keeps_the_local_file_and_the_accounts(harness, visitor, credentials_file):
    seed.seed(harness.db_path, credentials_file)
    before = credentials_file.read_bytes()

    seed.seed(harness.db_path, credentials_file)

    assert credentials_file.read_bytes() == before
    assert visitor.sign_in(accounts_in(credentials_file)[0]).status_code == 200


def test_seed_takes_accounts_from_an_existing_local_file_and_applies_a_changed_password(
    harness, visitor, credentials_file
):
    seed.seed(harness.db_path, credentials_file)
    old = accounts_in(credentials_file)[0]
    signed_in_before = Browser(harness)
    signed_in_before.sign_in(old)
    changed = Account(old.name, old.password[::-1] + "-changed")
    added = Account("duty.officer-3", "another generated-looking value")
    credentials_file.write_text(
        json.dumps({changed.name: changed.password, added.name: added.password}),
        encoding="utf-8",
    )

    names = seed.seed(harness.db_path, credentials_file)

    assert list(names) == [changed.name, added.name]
    assert visitor.sign_in(old).status_code == 401
    assert visitor.sign_in(changed).status_code == 200
    assert visitor.sign_in(added).status_code == 200
    # A session opened with the old password does not outlive it.
    assert signed_in_before.session() == {"signed_in": False}


@pytest.mark.parametrize(
    "content",
    [
        "not json",
        "[]",
        "{}",
        json.dumps({"officer1": TOO_SHORT}),
        json.dumps({"officer1": 12345678901234567890}),
        json.dumps({"Officer One": "long enough password value"}),
        json.dumps({"": "long enough password value"}),
    ],
)
def test_seed_refuses_a_local_file_it_cannot_use_and_creates_nothing(
    harness, visitor, credentials_file, content
):
    credentials_file.parent.mkdir(parents=True)
    credentials_file.write_text(content, encoding="utf-8")

    with pytest.raises(seed.SeedError):
        seed.seed(harness.db_path, credentials_file)

    assert credentials_file.read_text(encoding="utf-8") == content
    assert visitor.sign_in(Account("officer1", TOO_SHORT)).status_code == 401


def test_the_seed_command_says_where_the_passwords_are_without_printing_them(
    harness, visitor, credentials_file, capsys
):
    status = seed.main(["--db", str(harness.db_path), "--credentials", str(credentials_file)])

    printed = capsys.readouterr()
    accounts = accounts_in(credentials_file)
    assert status == 0
    assert str(credentials_file) in printed.out
    for account in accounts:
        assert account.name in printed.out
        assert account.password not in printed.out + printed.err
    assert visitor.sign_in(accounts[-1]).status_code == 200


def test_the_seed_command_reports_an_unusable_local_file(harness, credentials_file, capsys):
    credentials_file.parent.mkdir(parents=True)
    credentials_file.write_text(json.dumps({"officer1": TOO_SHORT}), encoding="utf-8")

    status = seed.main(["--db", str(harness.db_path), "--credentials", str(credentials_file)])

    printed = capsys.readouterr()
    assert status == 1
    assert "officer1" in printed.err
    assert TOO_SHORT not in printed.out + printed.err


def test_the_default_local_file_is_in_the_directory_git_ignores():
    assert seed.DEFAULT_CREDENTIALS.parent == seed.DEFAULT_DB.parent
    assert seed.DEFAULT_CREDENTIALS.parent.name == "data"
