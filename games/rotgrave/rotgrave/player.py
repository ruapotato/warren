"""ROTGRAVE -- the survivor, and the camera over their shoulder.

Third person, and zoomed in enough that the body reads. The reason
is the portal gun: a first-person view through a hole you made is a
view with no way to tell whether you went through it. Seeing your own
body arrive on the other side is the feedback that makes the
mechanic legible, and it only works if the body is big enough in
frame to watch.
"""

import math

import warren as wr


class Player:
    EYE = 1.62
    WALK = 4.6
    SPRINT = 7.2
    # How far back and up the camera sits, and how fast it catches
    # up. Instant is nauseating and slow is seasick; a tenth of a
    # second reads as a camera operator who is paying attention.
    BOOM = 4.2
    BOOM_UP = 1.15
    BOOM_LAG = 0.10

    def __init__(self, world):
        self.world = world
        self.body = None
        self.camera = None
        self.yaw = 0.0
        self.pitch = -0.12
        self._boom = wr.Vec3()
        self._captured = False

    def spawn(self, parent, at):
        self.body = wr.CharacterBody3D()
        self.body.name = "Survivor"
        self.body.radius = 0.4
        self.body.height = 1.8
        self.body.step_height = 0.45
        self.body.position = at
        parent.add_child(self.body)

        # A visible body, because a third-person camera with nothing
        # under it is a floating camera.
        mesh = wr.MeshBuilder()
        mesh.slot = 0
        mesh.add_box(wr.Vec3(0, 0.9, 0), wr.Vec3(0.55, 1.8, 0.35))
        shape = wr.MeshInstance3D()
        shape.name = "Body"
        shape.mesh = mesh.build()
        mat = wr.Material()
        mat.albedo = wr.Color(0.72, 0.68, 0.58, 1.0)
        mat.roughness = 0.8
        shape.set_material(0, mat)
        self.body.add_child(shape)

        self.camera = wr.Camera3D()
        self.camera.name = "Eye"
        parent.add_child(self.camera)
        self.camera.make_current()
        self._boom = at
        return self

    @property
    def position(self):
        return self.body.global_position if self.body else wr.Vec3()

    def look_direction(self):
        cp = math.cos(self.pitch)
        return wr.Vec3(-math.sin(self.yaw) * cp, math.sin(self.pitch),
                       -math.cos(self.yaw) * cp)

    def overhead(self, at):
        """Park the camera above the town and stop driving it."""
        self._overhead = True
        self.camera.position = at
        self.camera.euler = wr.Vec3(0.0, -1.35, 0.0)

    def update(self, dt, window_captured):
        if not self.body or getattr(self, "_overhead", False):
            return
        if window_captured:
            m = wr.mouse_motion()
            self.yaw -= m.x * 0.0022
            self.pitch = max(-1.35, min(1.15, self.pitch - m.y * 0.0022))

        # Movement is in the camera's frame with the pitch thrown
        # away, so looking at your feet does not walk you into the
        # ground.
        fwd = wr.Vec3(-math.sin(self.yaw), 0.0, -math.cos(self.yaw))
        right = wr.Vec3(math.cos(self.yaw), 0.0, -math.sin(self.yaw))
        wish = wr.Vec3()
        if wr.action_down("forward"):
            wish = wish + fwd
        if wr.action_down("back"):
            wish = wish - fwd
        if wr.action_down("right"):
            wish = wish + right
        if wr.action_down("left"):
            wish = wish - right
        if wish.length() > 0.001:
            wish = wish.normalized()

        speed = self.SPRINT if wr.action_down("sprint") else self.WALK
        v = self.body.velocity
        target = wish * speed
        # Ground acceleration is quick and air control is not, which
        # is the difference between a body with weight and a hovering
        # camera. Nothing here is a physics model; it is a feel.
        rate = 14.0 if self.body.is_on_floor() else 2.5
        k = min(1.0, rate * dt)
        v = wr.Vec3(v.x + (target.x - v.x) * k, v.y,
                    v.z + (target.z - v.z) * k)
        if self.body.is_on_floor() and wr.action_pressed("jump"):
            v = wr.Vec3(v.x, 6.0, v.z)
        self.body.velocity = v
        self.body.move_and_slide(self.world, dt)

        # The body faces where it is going, not where the camera
        # looks -- a survivor walking sideways while staring forward
        # is a strafing animation nobody has.
        if wish.length() > 0.001:
            self.body.euler = wr.Vec3(math.atan2(-wish.x, -wish.z), 0.0, 0.0)

        self._follow(dt)

    def _follow(self, dt):
        want = self.position + wr.Vec3(0.0, self.EYE, 0.0)
        k = 1.0 - math.exp(-dt / max(self.BOOM_LAG, 1e-4))
        self._boom = self._boom + (want - self._boom) * k

        look = self.look_direction()
        eye = self._boom - look * self.BOOM + wr.Vec3(0.0, self.BOOM_UP, 0.0)

        # Do not put the camera through a wall. Tracing from the head
        # outwards and stopping short is the whole of third-person
        # camera collision and the difference between a shooter and a
        # tour of the inside of a brick.
        hit = self.world.trace(self._boom, eye, 0xffffffff)
        if hit.get("hit"):
            p = hit["point"]
            n = hit.get("normal", wr.Vec3(0, 1, 0))
            eye = wr.Vec3(p.x, p.y, p.z) + n * 0.25

        self.camera.position = eye
        self.camera.euler = wr.Vec3(self.yaw, self.pitch, 0.0)
