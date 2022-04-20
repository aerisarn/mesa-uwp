from datetime import timedelta


class MesaCIException(Exception):
    pass


class MesaCITimeoutError(MesaCIException):
    def __init__(self, *args, timeout_duration: timedelta) -> None:
        super().__init__(*args)
        self.timeout_duration = timeout_duration


class MesaCIRetryError(MesaCIException):
    def __init__(self, *args, retry_count: int) -> None:
        super().__init__(*args)
        self.retry_count = retry_count


class MesaCIParseException(MesaCIException):
    pass
