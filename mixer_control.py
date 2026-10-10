"""Send effective routing changes once; keep liveness separate from settings."""
class MixerControl:
    def __init__(self):
        self.last_sent = None

    def reset(self):
        self.last_sent = None

    def apply(self, script, pids, gains):
        desired = tuple(sorted(zip(pids, gains)))
        if desired == self.last_sent:
            return False
        script.exports_sync.configure([p for p, _ in desired], [g for _, g in desired])
        # Failed RPCs are not cached: the next reconciliation must retry.
        self.last_sent = desired
        return True

    def health(self, script):
        script.exports_sync.heartbeat()
        return script.exports_sync.stats()
