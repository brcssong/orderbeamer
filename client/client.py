import struct
import socket
import time
import threading
from decimal import Decimal

def maxc(bits):
    return 2 ** bits - 1

SOCK_PATH = '/tmp/orderbeamer.sock'
MAX_SESSIONS_EXP = 24 # Allows for storing of up to 2^{MAX_SESSIONS_EXP} sessions
SIXTYFOUR = 64
NON_SESSIONS_EXP = SIXTYFOUR - MAX_SESSIONS_EXP
assert MAX_SESSIONS_EXP < SIXTYFOUR

# struct Order {
#     timestamp: u64, /* when picked up on C++ side */
#     true_timestamp: u64, /* when inserted
#     user_id: u64,
#     order_id: u64,
#     price: i64,
#     qty: u32,
#     prev: u32, /* for price-level tree tracking */
#     next: u32, /* for price-level tree tracking */
#     price_level_arena_idx: u32, /* for further tracking */
#     is_buy: bool,
# }

ORDER_FMT_STR = '@QQQQqIIII?'
PADDING_NEEDED = 64 - struct.calcsize(ORDER_FMT_STR)
assert PADDING_NEEDED >= 0
FMT_STR = f'{ORDER_FMT_STR}{PADDING_NEEDED}x'

def create_cancel_order(user_id: int, order_id: int) -> bytes:
    # A cancel just re-sends a live order_id from its owner; the engine ignores price/qty.
    return struct.pack(FMT_STR, 0xFFFFFFFF, abs(time.time_ns()), user_id, order_id, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, True)

def create_order(user_id: int, price: float, quantity: int, order_id: int, is_buy: bool) -> bytes:
    assert price > 0.0, "CreateOrder: Price is nonpositive."
    price = float(f"{price:.2f}")
    assert quantity > 0, "CreateOrder: Quantity is nonpositive."
    reinterpret_price = int(Decimal(price) * Decimal(10 ** 9))
    return struct.pack(FMT_STR, 0xFFFFFFFF, abs(time.time_ns()), user_id, order_id, reinterpret_price, quantity, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, is_buy)

def connect(path=SOCK_PATH, timeout=5.0):
    deadline = time.time() + timeout
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    while time.time() < deadline:
        try:
            sock.connect(path)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.05)
    raise TimeoutError(f"ERROR: Gateway never came up at {path}")

class User:
    def __init__(self, uid: int):
        self.user_id = uid

class Client:
    _lock = threading.Lock()
    session_counter = 0
    session_history = []

    def __enter__(self):
        # With default spec, stores up to 2^24 unique sessions for the client and 2^40 max orders can process in each session
        with self._lock:
            Client.session_counter += 1
            self.session_counter = Client.session_counter
        self.order_counter = 0
        self.order_history = []
        self.order_output = []
        self.user_counter = 0
        # Connects to the socket gateway
        self.socket = connect()
        return self

    def register_user(self):
        self.user_counter += 1
        new_user_id = (self.session_counter << NON_SESSIONS_EXP) | self.user_counter
        assert self.session_counter <= maxc(MAX_SESSIONS_EXP)
        assert self.user_counter <= maxc(NON_SESSIONS_EXP)
        return User(new_user_id)

    def __generate_order_id(self) -> int:
        self.order_counter += 1
        new_order_id = (self.session_counter << NON_SESSIONS_EXP) | self.order_counter
        assert self.session_counter <= maxc(MAX_SESSIONS_EXP)
        assert self.order_counter <= maxc(NON_SESSIONS_EXP)
        return new_order_id

    def cancel_order(self, user: User, order_id: int):
        order_obj = create_cancel_order(user.user_id, order_id)
        self.socket.send(order_obj)

    def send_order(self, user: User, price: float, quantity: int, is_buy: bool) -> int:
        # Sends an order through the gateway and adds responses to Order History + Order Output
        oid = self.__generate_order_id()
        order_obj = create_order(user.user_id, price, quantity, oid, is_buy)
        # Send it through gateway
        self.socket.send(order_obj)
        return oid

    def __exit__(self, exc_type, exc_val, exc_tb):
        # End session: append to session_history
        Client.session_history.append({
            'num_orders_processed': self.order_counter,
            'submitted_order_history': self.order_history,
            'order_response': self.order_output,
        })
        self.socket.close()

if __name__ == '__main__':
    import sys
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100_000
    print(">> LOADING CLIENT")
    with Client() as c:
        print(f">> CONNECTED. Sending {n} ping-pong orders over the gateway...")
        buyer, seller = c.register_user(), c.register_user()
        t0 = time.perf_counter()
        for i in range(n):
            if i % 2 == 0:
                c.send_order(buyer, 1.00, 1, True)
            else:
                c.send_order(seller, 1.00, 1, False)
        dt = time.perf_counter() - t0
        print(f">> SENT {n} orders in {dt:.4f}s -> {n / dt / 1e6:.3f} M orders/sec")
    print(">> DONE")