"""Vector pinout diagrams traced from the supplied board photos."""
from reportlab.platypus import Flowable
from reportlab.lib.colors import HexColor, white

INK = '#192F40'
TEAL = '#117E88'
GRAY = '#A4B1BB'
RED = '#C72E3C'
BLACK = '#202B35'
BLUE = '#255BC3'
GREEN = '#218565'
YELLOW = '#987100'
SENSE = '#A85413'


class Pinout(Flowable):
    def __init__(self, kind, width):
        super().__init__()
        self.kind, self.width = kind, width
        self.height = 470 if kind == 'esp32' else 550

    def text(self, x, y, s, size=10, color=INK, bold=False, align='left'):
        c = self.canv
        c.setFillColor(HexColor(color))
        c.setFont('MalgunBold' if bold else 'Malgun', size)
        getattr(c, {'left': 'drawString', 'center': 'drawCentredString', 'right': 'drawRightString'}[align])(x, y, s)

    def line(self, x1, y1, x2, y2, color=INK, width=1.5):
        self.canv.setStrokeColor(HexColor(color))
        self.canv.setLineWidth(width)
        self.canv.line(x1, y1, x2, y2)

    def box(self, x, y, w, h, fill='#F0F5F7', stroke='#D6E2E8'):
        c = self.canv
        c.setFillColor(HexColor(fill))
        c.setStrokeColor(HexColor(stroke))
        c.setLineWidth(1)
        c.roundRect(x, y, w, h, 5, fill=1, stroke=1)

    def pin(self, x, y, color, radius=4):
        c = self.canv
        c.setFillColor(HexColor(color))
        c.circle(x, y, radius, stroke=0, fill=1)

    def draw(self):
        if self.kind == 'esp32':
            self.esp32()
        else:
            self.connectors()

    def esp32(self):
        self.text(255, 450, '부품이 보이는 앞면 / USB 단자가 아래', 12, TEAL, True, 'center')
        self.box(178, 39, 158, 394)
        self.box(230, 345, 55, 74, '#E0E8EC')
        self.text(257, 388, '안테나', 9, align='center')
        self.text(257, 365, 'ESP32', 10, bold=True, align='center')
        self.box(237, 35, 42, 29, '#DAE2E6', TEAL)
        self.text(258, 44, 'USB', 10, bold=True, align='center')
        self.text(199, 49, 'RST', 8)
        self.text(330, 49, 'BOOT', 8, align='right')
        left = ['3V3', 'EN', 'SP', 'SN', 'G34', 'G35', 'G32', 'G33', 'G25', 'G26', 'G27', 'G14', 'G12', 'GND', 'G13', 'SD2', 'SD3', 'CMD', 'V5']
        right = ['GND', 'G23', 'G22', 'TXD', 'RXD', 'G21', 'GND', 'G19', 'G18', 'G5', 'G17', 'G16', 'G4', 'G0', 'G2', 'G15', 'SD1', 'SD0', 'CLK']
        lc = {4: SENSE, 18: RED}
        rc = {0: BLUE, 1: GREEN, 5: RED, 6: BLACK, 7: YELLOW, 8: '#6650A4'}
        for i, (a, b) in enumerate(zip(left, right)):
            y = 420 - i * 19
            self.pin(178, y, lc.get(i, GRAY))
            self.pin(336, y, rc.get(i, GRAY))
            if i in lc:
                self.text(190, y - 3, a, 9, lc[i], True)
            if i in rc:
                self.text(324, y - 3, b, 9, rc[i], True, 'right')
        for i, label in [(0, 'PSU GND (파랑)'), (1, 'PSU PSON (초록)'), (5, 'LED + (빨강)'), (6, 'LED - (검정)'), (7, '접점 (노랑)'), (8, '접점 (흰색)')]:
            y = 420 - i * 19
            self.line(340, y, 382, y, rc[i], 2)
            self.text(389, y - 3, label, 9, rc[i], True)
        self.text(390, 337, '스위치 1×4', 10, TEAL, True)
        self.line(375, 331, 375, 260, TEAL, 1)
        y = 420 - 4 * 19
        self.line(8, y, 174, y, SENSE, 2)
        self.text(8, y + 9, 'TPMS1 9번 → G34', 10, SENSE, True)
        self.text(8, y - 18, '검은 감지 케이블', 9)
        y = 420 - 18 * 19
        self.line(8, y, 174, y, RED, 2)
        self.text(8, y + 11, 'PSU 5VSB → V5', 10, RED, True)
        self.text(8, y - 19, 'PSU 케이블의 빨간 선', 9)

    def connectors(self):
        self.text(0, 532, 'A. 스위치 1×4: 도선과 핀을 맞추기', 12, TEAL, True)
        self.text(0, 513, 'ESP32 앞면에서 USB가 아래일 때, 오른쪽 핀 열의 위 → 아래 순서', 9)
        names = [('G21', '빨강', 'LED +', RED), ('GND', '검정', 'LED -', BLACK), ('G19', '노랑', '접점', YELLOW), ('G18', '흰색', '접점', '#6650A4')]
        self.box(34, 374, 69, 121, '#E7EEF1', TEAL)
        for i, (pin, wire, role, color) in enumerate(names):
            y = 478 - i * 27
            self.text(69, y - 3, pin, 11, color, True, 'center')
            self.line(105, y, 213, y, color, 3)
            self.text(225, y - 3, f'{wire} 도선  /  {role}', 10, color, True)
        self.text(351, 447, '1×4 커넥터', 10, TEAL, True)
        self.text(351, 428, '네 핀을 한 번에 연결', 9)
        self.text(0, 352, '빨간 선이 G21, 검은 선이 GND에 오도록 1×4 커넥터를 꽂으세요.', 10)
        self.line(0, 337, 511, 337, '#D6E2E8', 1)
        self.text(0, 314, 'B. BC250 TPMS1: 9번만 연결', 12, TEAL, True)
        self.text(0, 295, '설치 사진과 같은 방향: 핀 1 삼각형이 위쪽, LAN 포트는 오른쪽', 9)
        self.box(203, 22, 96, 246)
        self.box(330, 188, 112, 77, '#E1E8EC')
        self.text(386, 221, 'LAN 포트', 12, align='center')
        self.text(208, 279, '▼ 핀 1 표시', 10, TEAL, True)
        for row in range(9):
            for col in range(2):
                num = row * 2 + col + 1
                x, y = 226 + col * 49, 248 - row * 26
                if num == 13:
                    continue
                color = SENSE if num == 9 else GRAY
                self.pin(x, y, color, 10)
                if num in (1, 9):
                    self.text(x, y - 3, str(num), 9, '#FFFFFF' if num == 9 else INK, num == 9, 'center')
        self.line(12, 144, 214, 144, SENSE, 3)
        self.text(12, 162, 'ESP32 G34', 12, SENSE, True)
        self.text(12, 124, '검은 감지선 → 9번', 10, SENSE, True)
        self.text(325, 132, '삼각형 쪽에서 시작해', 10)
        self.text(325, 112, '왼쪽 열의 다섯 번째에 연결', 10, SENSE, True)
