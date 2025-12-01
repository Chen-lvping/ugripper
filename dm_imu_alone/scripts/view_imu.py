import socket
import struct
import pygame
from pygame.locals import *
from OpenGL.GL import *
from OpenGL.GLU import *
import math

# --- 配置 ---
UDP_IP = "127.0.0.1"
UDP_PORT = 9999


# --- 四元数转旋转矩阵 ---
def quat_to_matrix(w, x, y, z):
    # 归一化，防止数据漂移导致形变
    l = math.sqrt(w * w + x * x + y * y + z * z)
    if l == 0:
        return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    w, x, y, z = w / l, x / l, y / l, z / l

    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz, wx, wy, wz = x * y, x * z, y * z, w * x, w * y, w * z

    return [
        1 - 2 * (yy + zz),
        2 * (xy + wz),
        2 * (xz - wy),
        0,
        2 * (xy - wz),
        1 - 2 * (xx + zz),
        2 * (yz + wx),
        0,
        2 * (xz + wy),
        2 * (yz - wx),
        1 - 2 * (xx + yy),
        0,
        0,
        0,
        0,
        1,
    ]


# --- 定义方块的顶点和边 ---
verticies = (
    (1, -1, -1),
    (1, 1, -1),
    (-1, 1, -1),
    (-1, -1, -1),
    (1, -1, 1),
    (1, 1, 1),
    (-1, -1, 1),
    (-1, 1, 1),
)
edges = (
    (0, 1),
    (0, 3),
    (0, 4),
    (2, 1),
    (2, 3),
    (2, 7),
    (6, 3),
    (6, 4),
    (6, 7),
    (5, 1),
    (5, 4),
    (5, 7),
)
colors = (
    (1, 0, 0),
    (0, 1, 0),
    (0, 0, 1),
    (1, 1, 0),
    (1, 0, 1),
    (0, 1, 1),
    (1, 0, 0),
    (0, 1, 0),
    (0, 0, 1),
    (1, 1, 0),
    (1, 1, 1),
    (0, 1, 1),
)


def draw_cube():
    glBegin(GL_LINES)
    for edge in edges:
        for vertex in edge:
            glColor3fv(colors[vertex])  # 给顶点上色方便分清方向
            glVertex3fv(verticies[vertex])
    glEnd()


# --- 主程序 ---
def main():
    # 1. 初始化 UDP
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, UDP_PORT))
    sock.setblocking(0)  # 设置非阻塞模式

    # 2. 初始化 Pygame 和 OpenGL
    pygame.init()
    display = (800, 600)
    pygame.display.set_mode(display, DOUBLEBUF | OPENGL)
    pygame.display.set_caption("IMU 3D Visualizer")

    gluPerspective(45, (display[0] / display[1]), 0.1, 50.0)
    glTranslatef(0.0, 0.0, -5)  # 往后退一点，才能看到方块

    current_quat = (1, 0, 0, 0)  # w, x, y, z

    while True:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                pygame.quit()
                quit()

        # 3. 接收网络数据
        try:
            while True:  # 循环读完缓冲区，只取最新的
                data, _ = sock.recvfrom(1024)
                # 解析 C++ 发来的 4个 float
                if len(data) == 16:
                    current_quat = struct.unpack("ffff", data)
        except BlockingIOError:
            pass  # 没有新数据，继续渲染

        # 4. 渲染
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)

        glPushMatrix()  # 保存当前坐标系

        # 计算旋转矩阵并应用
        # 注意：C++发来的是 w,x,y,z，但 OpenGL 矩阵需要列主序
        w, x, y, z = current_quat
        rot_matrix = quat_to_matrix(w, x, y, z)
        glMultMatrixf(rot_matrix)

        draw_cube()  # 画方块

        glPopMatrix()  # 恢复坐标系

        pygame.display.flip()
        pygame.time.wait(10)


if __name__ == "__main__":
    main()
