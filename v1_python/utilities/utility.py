"""
General utility functions for the trading system.
Includes symbol handling, file operations, math utilities, and market data processing.

"""

import json
import sys
from collections.abc import Callable
from datetime import UTC, datetime, timedelta
from decimal import Decimal
from math import ceil, floor
from pathlib import Path
from typing import Any

import exchange_calendars
import yaml


# File system operations
def _get_trader_dir(temp_name: str) -> tuple[Path, Path]:
    """Get path where trader is running in."""
    cwd: Path = Path.cwd()
    temp_path: Path = cwd.joinpath(temp_name)

    if temp_path.exists():
        return cwd, temp_path

    home_path: Path = Path.home()
    temp_path = home_path.joinpath(temp_name)

    if not temp_path.exists():
        temp_path.mkdir()

    return home_path, temp_path


TRADER_DIR, TEMP_DIR = _get_trader_dir(".vntrader")
sys.path.append(str(TRADER_DIR))


def get_file_path(filename: str) -> Path:
    """Get path for file in the 'setting' folder."""
    setting_folder = Path(__file__).parent.parent.joinpath("setting")
    if not setting_folder.exists():
        setting_folder.mkdir()
    return setting_folder.joinpath(filename)


def get_folder_path(folder_name: str) -> Path:
    """Get path for temp folder with folder name."""
    folder_path: Path = TEMP_DIR.joinpath(folder_name)
    if not folder_path.exists():
        folder_path.mkdir()
    return folder_path


# JSON operations
def load_json(filename: str) -> dict:
    """Load data from json file in temp path."""
    filepath: Path = get_file_path(filename)

    if filepath.exists():
        with open(filepath, encoding="UTF-8") as f:
            data: dict = json.load(f)
        return data
    else:
        save_json(filename, {})
        return {}


def save_json(filename: str, data: dict) -> None:
    """Save data into json file in temp path."""
    filepath: Path = get_file_path(filename)
    with open(filepath, mode="w+", encoding="UTF-8") as f:
        json.dump(data, f, indent=4, ensure_ascii=False)


# YAML operations
def load_yaml(filename: str) -> dict[str, Any]:
    """Load data from YAML file in setting path."""
    filepath: Path = get_file_path(filename)

    if filepath.exists():
        with open(filepath, encoding="UTF-8") as f:
            data = yaml.safe_load(f)
        return data if isinstance(data, dict) else {}
    else:
        save_yaml(filename, {})
        return {}


def save_yaml(filename: str, data: dict) -> None:
    """Save data into YAML file in setting path."""
    filepath: Path = get_file_path(filename)

    # Add metadata for better human readability
    enhanced_data = {
        "metadata": {
            "version": "1.0",
            "created_at": datetime.now().isoformat(),
            "schema_version": "portfolio_v1",
            "description": "Portfolio strategy data",
        },
        "data": data,
    }

    with open(filepath, mode="w+", encoding="UTF-8") as f:
        yaml.dump(enhanced_data, f, default_flow_style=False, sort_keys=False, allow_unicode=True, indent=2)


def load_yaml_data(filename: str) -> dict[str, Any]:
    """Load only the data portion from YAML file, excluding metadata."""
    full_data = load_yaml(filename)
    data = full_data.get("data", {})
    return data if isinstance(data, dict) else {}


# Math utilities
def round_to(value: float, target: float) -> float:
    """Round price to price tick value."""
    decimal_value: Decimal = Decimal(str(value))
    decimal_target: Decimal = Decimal(str(target))
    rounded: float = float(int(round(decimal_value / decimal_target)) * decimal_target)
    return rounded


def floor_to(value: float, target: float) -> float:
    """Similar to math.floor function, but to target float number."""
    decimal_value: Decimal = Decimal(str(value))
    decimal_target: Decimal = Decimal(str(target))
    result: float = float(int(floor(decimal_value / decimal_target)) * decimal_target)
    return result


def ceil_to(value: float, target: float) -> float:
    """Similar to math.ceil function, but to target float number."""
    decimal_value: Decimal = Decimal(str(value))
    decimal_target: Decimal = Decimal(str(target))
    result: float = float(int(ceil(decimal_value / decimal_target)) * decimal_target)
    return result


def get_digits(value: float) -> int:
    """Get number of digits after decimal point."""
    value_str: str = str(value)

    if "e-" in value_str:
        _, buf = value_str.split("e-")
        return int(buf)
    elif "." in value_str:
        _, buf = value_str.split(".")
        return len(buf)
    else:
        return 0


# Decorator utilities
def virtual(func: Callable) -> Callable:
    """Mark a function as virtual, allowing override in subclasses."""
    return func


# Calendar constants
ANNUAL_DAYS = 240

# Exchange calendar setup
cn_calendar: exchange_calendars.ExchangeCalendar = exchange_calendars.get_calendar("XSHG")
holidays: list = [x.to_pydatetime() for x in cn_calendar.precomputed_holidays()]
start: datetime = datetime.today()
PUBLIC_HOLIDAYS = [x for x in holidays if x >= start]


def calculate_days_to_expiry(option_expiry: datetime | None) -> int:
    """Calculate trading days to expiry, excluding weekends and holidays.

    If option_expiry is None, return 0 to indicate unknown/expired.
    """
    if option_expiry is None:
        return 0

    # Convert to timezone-naive datetime for comparison
    current_dt: datetime = datetime.now(UTC).replace(tzinfo=None).replace(hour=0, minute=0, second=0, microsecond=0)
    option_expiry_naive = option_expiry.replace(tzinfo=None) if option_expiry.tzinfo else option_expiry

    days: int = 1

    while current_dt < option_expiry_naive:
        current_dt += timedelta(days=1)
        if current_dt.weekday() in [5, 6] or current_dt in PUBLIC_HOLIDAYS:
            continue
        days += 1

    return days
