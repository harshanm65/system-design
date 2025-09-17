# async_iter_mvp.py  (Python 3.10+)
# Minimal async, pause/resume-able composite iterator over multiple files.

from __future__ import annotations
import asyncio
import json
from dataclasses import dataclass, asdict
from typing import Optional, List, Any


# --------- low-level async file read (non-blocking to event loop) ---------
def _read_chunk(path: str, offset: int, nbytes: int) -> bytes:
    with open(path, "rb") as f:
        f.seek(offset)
        data = f.read(nbytes)
    return data


async def read_chunk_async(path: str, offset: int, nbytes: int) -> bytes:
    # Use a threadpool transparently; lets us await blocking I/O.
    return await asyncio.to_thread(_read_chunk, path, offset, nbytes)


# --------------------- serializable per-file iterator state ----------------
@dataclass
class FileIterState:
    path: str
    offset: int = 0
    done: bool = False

    def to_json(self) -> str:
        return json.dumps(asdict(self), separators=(",", ":"))

    @staticmethod
    def from_json(s: str) -> "FileIterState":
        d = json.loads(s)
        return FileIterState(**d)


# --------------------------- async per-file iterator -----------------------
class AsyncFileIterator:
    def __init__(self, path: str, chunk_size: int = 64) -> None:
        self._state = FileIterState(path=path, offset=0, done=False)
        self._chunk = chunk_size

    @property
    def path(self) -> str:
        return self._state.path

    async def next(self) -> Optional[bytes]:
        if self._state.done:
            return None
        data = await read_chunk_async(self._state.path, self._state.offset, self._chunk)
        if not data:
            self._state.done = True
            return None
        self._state.offset += len(data)
        return data

    def get_state(self) -> str:
        return self._state.to_json()

    def set_state(self, state_json: str) -> None:
        self._state = FileIterState.from_json(state_json)


# ----------------------------- composite iterator --------------------------
@dataclass
class CompositeItem:
    source: str
    sequence: int
    data: bytes


class CompositeIterator:
    """
    Fan-in iterator over multiple AsyncFileIterator sources.
    Pumps each source concurrently and yields CompositeItem from a queue.
    Single-consumer design.
    """
    def __init__(self, paths: List[str], chunk_size: int = 64) -> None:
        self._iters = [AsyncFileIterator(p, chunk_size) for p in paths]
        self._q: asyncio.Queue[Optional[CompositeItem]] = asyncio.Queue()
        self._active = len(self._iters)
        self._closed = False
        self._lock = asyncio.Lock()
        self._tasks: List[asyncio.Task[Any]] = []
        self._start_pumps()

    @classmethod
    def from_states(cls, states: List[str], chunk_size: int = 64) -> "CompositeIterator":
        iters = [FileIterState.from_json(s) for s in states]
        obj = cls.__new__(cls)  # bypass __init__
        obj._iters = [AsyncFileIterator(st.path, chunk_size) for st in iters]
        for it, st in zip(obj._iters, iters):
            it.set_state(st.to_json())
        obj._q = asyncio.Queue()
        obj._active = len(obj._iters)
        obj._closed = False
        obj._lock = asyncio.Lock()
        obj._tasks = []
        obj._start_pumps()
        return obj

    def _start_pumps(self) -> None:
        for idx in range(len(self._iters)):
            self._tasks.append(asyncio.create_task(self._pump(idx)))

    async def _producer_done(self) -> None:
        async with self._lock:
            self._active -= 1
            if self._active == 0 and not self._closed:
                self._closed = True
                await self._q.put(None)  # sentinel for single-consumer

    async def _pump(self, idx: int) -> None:
        seq = 0
        try:
            while True:
                chunk = await self._iters[idx].next()
                if chunk is None:
                    break
                await self._q.put(CompositeItem(
                    source=self._iters[idx].path,
                    sequence=seq,
                    data=chunk
                ))
                seq += 1
        finally:
            await self._producer_done()

    async def next(self) -> Optional[CompositeItem]:
        item = await self._q.get()
        # None sentinel means all producers finished & queue drained.
        return item

    def get_state(self) -> List[str]:
        return [it.get_state() for it in self._iters]

    async def close(self) -> None:
        # Best-effort stop: cancel pumps and close queue for the single consumer.
        async with self._lock:
            if self._closed:
                return
            self._closed = True
            for t in self._tasks:
                t.cancel()
            await self._q.put(None)

    # Async-iterator sugar
    def __aiter__(self) -> "CompositeIterator":
        return self

    async def __anext__(self) -> CompositeItem:
        item = await self.next()
        if item is None:
            raise StopAsyncIteration
        return item


# ------------------------------ Demo / CLI ---------------------------------
async def demo_full_read(paths: List[str], chunk_size: int = 64) -> None:
    comp = CompositeIterator(paths, chunk_size)
    async for item in comp:
        print(f"[{item.source} /#{item.sequence}] {item.data.decode('utf-8', errors='replace')}", end="")


async def demo_pause_resume(paths: List[str], chunk_size: int = 64, first_n: int = 4) -> None:
    print("== First pass (read a few chunks) ==")
    comp = CompositeIterator(paths, chunk_size)
    count = 0
    async for item in comp:
        print(f"[{item.source} /#{item.sequence}] {item.data.decode('utf-8', errors='replace')}", end="")
        count += 1
        if count >= first_n:
            states = comp.get_state()
            await comp.close()      # stop pumps; snapshot taken
            break

    print("\n== Resuming from saved state ==")
    comp2 = CompositeIterator.from_states(states, chunk_size)
    async for item in comp2:
        print(f"[{item.source} /#{item.sequence}] {item.data.decode('utf-8', errors='replace')}", end="")
    print()


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <file1> [file2 ...]")
        sys.exit(1)
    files = sys.argv[1:]
    # Choose ONE of the demos below:
    # asyncio.run(demo_full_read(files, chunk_size=64))
    asyncio.run(demo_pause_resume(files, chunk_size=64, first_n=4))
