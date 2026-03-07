from abc import ABC, abstractmethod
from typing import TYPE_CHECKING, TypeVar

from engines.engine_event import EventEngine

if TYPE_CHECKING:
    from engines.engine_main import MainEngine

EngineType = TypeVar("EngineType", bound="BaseEngine")


class BaseEngine(ABC):
    """
    Abstract class for implementing a function engine.
    """

    @abstractmethod
    def __init__(self, main_engine: "MainEngine", event_engine: EventEngine, engine_name: str) -> None:
        """"""
        self.main_engine: "MainEngine" = main_engine
        self.event_engine: EventEngine = event_engine
        self.engine_name: str = engine_name

    def close(self) -> None:
        """"""
        return
