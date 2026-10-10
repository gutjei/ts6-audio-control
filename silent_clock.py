"""Dedicated process with no render streams or children, used as a silent clock."""
import threading
threading.Event().wait()
