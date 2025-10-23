import sys
from collections.abc import Callable
from datetime import datetime
from logging import CRITICAL, DEBUG, ERROR, INFO, WARNING
from pathlib import Path
from typing import TYPE_CHECKING, Any

from loguru import logger

from utilities.base_engine import BaseEngine
from utilities.event import EVENT_LOG
from utilities.object import LogData
from utilities.utility import get_folder_path

from .engine_event import Event, EventEngine

if TYPE_CHECKING:
    from .engine_main import MainEngine


__all__ = ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL", "logger", "LogEngine"]


# =============================================================================
# LOG SETTINGS
# =============================================================================

# Log configuration
LOG_ACTIVE: bool = True
LOG_LEVEL: int = DEBUG
LOG_CONSOLE: bool = True
LOG_FILE: bool = True

# Log format
LOG_FORMAT: str = (
    "<green>{time:DD/MM HH:mm:ss}</green> " "| <level>{level:<7}</level> " "| <cyan>{extra[gateway_name]:<10}</cyan> " "| {message}"
)


# Remove default stderr output
logger.remove()

# Global UI log handler
ui_log_handler = None

# Global WebSocket log handler
websocket_log_handler = None


def set_websocket_log_handler(handler: Callable[[Any], None]) -> None:
    """Set the WebSocket log handler for direct loguru routing."""
    global websocket_log_handler
    websocket_log_handler = handler


def websocket_sink(message: Any) -> None:
    """Custom sink for routing loguru logs directly to WebSocket."""
    if websocket_log_handler:
        websocket_log_handler(message.record)


# Add console output
if LOG_CONSOLE:
    logger.add(sink=sys.stdout, level=LOG_LEVEL, format=LOG_FORMAT)

# Add file output
if LOG_FILE:
    today_date: str = datetime.now().strftime("%Y%m%d")
    filename: str = f"{today_date}.log"
    log_path: Path = get_folder_path("log")
    file_path: Path = log_path.joinpath(filename)

    logger.add(sink=file_path, level=LOG_LEVEL, format=LOG_FORMAT)


# Add WebSocket output (will be added when backend is initialized)
def add_websocket_sink() -> None:
    """Add WebSocket sink to loguru logger."""
    logger.add(sink=websocket_sink, level=LOG_LEVEL, format=LOG_FORMAT, serialize=False)


class LogEngine(BaseEngine):
    """
    Provides log event output function.
    """

    level_map: dict[int, str] = {DEBUG: "DEBUG", INFO: "INFO", WARNING: "WARNING", ERROR: "ERROR", CRITICAL: "CRITICAL"}

    def __init__(self, main_engine: "MainEngine", event_engine: EventEngine) -> None:
        """Initialize LogEngine"""
        super().__init__(main_engine, event_engine, "log")

        self.active = LOG_ACTIVE
        self.register_log(EVENT_LOG)

    def process_log_event(self, event: Event) -> None:
        """Process log event"""
        if not self.active:
            return

        log: LogData = event.data
        level: str | int = self.level_map.get(log.level, log.level)
        logger.log(level, log.msg, gateway_name=log.gateway_name, time=log.time)

    def register_log(self, event_type: str) -> None:
        """Register log event handler"""
        self.event_engine.register(event_type, self.process_log_event)
