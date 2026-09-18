# Warren -- generated type stubs. Do not edit.
#
# Written from the engine's own class registry by `warren --stubs`,
# so this file describes exactly the classes, methods and properties
# the running engine exposes -- plugins included, if they were loaded
# when it was generated. Regenerate it when the engine changes.
#
# Put it next to your scripts, or anywhere on the type checker's path,
# and an editor will complete `mf.` correctly.

from types import SimpleNamespace
from typing import Any

version: str
Key: SimpleNamespace

# ------------------------------------------------------------ values
#
# Small value types. They are copied, not referenced: reading
# `node.position` gives a Vec3 that is not the node's, so mutating it
# changes nothing -- assign the whole property back.

class _Value:
    def __init__(self, *args: Any) -> None: ...
    def as_tuple(self) -> tuple[float, ...]: ...
    def __eq__(self, other: object) -> bool: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...

class _Vector(_Value):
    def length(self) -> float: ...
    def normalized(self) -> Any: ...
    def dot(self, other: Any) -> float: ...
    def cross(self, other: Any) -> Any: ...
    def __add__(self, other: Any) -> Any: ...
    def __sub__(self, other: Any) -> Any: ...
    def __mul__(self, other: Any) -> Any: ...
    def __rmul__(self, other: Any) -> Any: ...
    def __truediv__(self, other: Any) -> Any: ...
    def __neg__(self) -> Any: ...

class Vec2(_Vector):
    x: float
    y: float
    def __init__(self, x: float = 0, y: float = 0) -> None: ...

class Vec3(_Vector):
    x: float
    y: float
    z: float
    def __init__(self, x: float = 0, y: float = 0, z: float = 0) -> None: ...

class Vec4(_Vector):
    x: float
    y: float
    z: float
    w: float
    def __init__(self, x: float = 0, y: float = 0, z: float = 0,
                 w: float = 0) -> None: ...

class Color(_Vector):
    r: float
    g: float
    b: float
    a: float
    def __init__(self, r: float = 0, g: float = 0, b: float = 0,
                 a: float = 1) -> None: ...

class Quat(_Value):
    x: float
    y: float
    z: float
    w: float
    def __init__(self, x: float = 0, y: float = 0, z: float = 0,
                 w: float = 1) -> None: ...

class Basis(_Value):
    def __init__(self) -> None: ...

class Transform3D(_Value):
    origin: Vec3
    basis: Basis
    def __init__(self, origin_or_basis: Any = ..., origin: Vec3 = ...) -> None: ...

class Plane(_Value):
    normal: Vec3
    d: float
    def __init__(self, normal: Vec3 = ..., d: float = 0) -> None: ...

class AABB(_Value):
    min: Vec3
    max: Vec3
    def __init__(self, lo: Vec3 = ..., hi: Vec3 = ...) -> None: ...

class Rect2(_Value):
    x: float
    y: float
    width: float
    height: float
    def __init__(self, x: float = 0, y: float = 0, w: float = 0,
                 h: float = 0) -> None: ...

class Projection(_Value):
    def __init__(self) -> None: ...

# ----------------------------------------------------------- classes

class Object:
    pass

class Node(Object):
    scene_path: str
    process: bool
    physics_process: bool

    def __init__(self) -> None: ...
    def set_name(self, name: str) -> None: ...
    def get_name(self) -> str: ...
    def get_path(self) -> str: ...
    def add_child(self, child: Node | None) -> None: ...
    def remove_child(self, child: Node | None) -> None: ...
    def queue_free(self) -> None: ...
    def get_parent(self) -> Node | None: ...
    def get_owner(self) -> Node | None: ...
    def set_owner(self, owner: Node | None) -> None: ...
    def get_child_count(self) -> int: ...
    def get_child(self, index: int) -> Node | None: ...
    def find_child(self, name: str) -> Node | None: ...
    def get_node(self, path: str) -> Node | None: ...
    def find_path(self, path: str) -> Node | None: ...
    def find_by_class(self, class_name: str) -> Node | None: ...
    def add_to_group(self, group: str) -> None: ...
    def remove_from_group(self, group: str) -> None: ...
    def is_in_group(self, group: str) -> bool: ...
    def is_inside_tree(self) -> bool: ...
    def print_tree(self, indent: int = 0) -> str: ...
    # signals: tree_entered, tree_exiting

class PhysicsWorld(Object):
    max_portal_hops: int

    def __init__(self) -> None: ...
    def trace(self, from_point: Vec3, to_point: Vec3, mask: int = 4294967295) -> dict: ...
    def raycast(self, from_point: Vec3, to_point: Vec3, mask: int = 4294967295) -> dict: ...
    def add_sphere(self, at: Vec3, radius: float, layer: int = 1) -> int: ...
    def add_box(self, at: Transform3D, half_extents: Vec3, layer: int = 1) -> int: ...
    def add_capsule(self, at: Transform3D, radius: float, height: float, layer: int = 1) -> int: ...
    def add_portal(self, portal: Portal3D | None) -> None: ...
    def remove_portal(self, portal: Portal3D | None) -> None: ...
    def remove_collider(self, collider: int) -> None: ...
    def set_collider_transform(self, collider: int, transform: Transform3D) -> None: ...
    def collider_count(self) -> int: ...
    def report(self) -> str: ...

class Resource(Object):
    resource_path: str
    resource_name: str

class Theme(Object):
    background: Color
    panel: Color
    text: Color
    text_dim: Color
    accent: Color
    control: Color
    control_hover: Color
    control_pressed: Color
    border: Color
    corner_radius: float  # range:0,16
    padding: float  # range:0,32
    separation: float  # range:0,32
    font_scale: float  # range:1,6
    border_width: float  # range:0,8
    def __init__(self) -> None: ...

class AudioClip(Resource):
    def __init__(self) -> None: ...
    def duration(self) -> float: ...
    def frames(self) -> int: ...

class AudioPlayer(Node):
    volume: float  # range:0,4
    pitch: float  # range:0.25,4
    loop: bool
    autoplay: bool

    def __init__(self) -> None: ...
    def play(self) -> None: ...
    def stop(self) -> None: ...
    def is_playing(self) -> bool: ...

class Control(Node):
    anchor_left: float  # range:0,1
    anchor_top: float  # range:0,1
    anchor_right: float  # range:0,1
    anchor_bottom: float  # range:0,1
    offset_left: float
    offset_top: float
    offset_right: float
    offset_bottom: float
    size_flags_horizontal: int
    size_flags_vertical: int
    stretch_ratio: float  # range:0,8
    custom_minimum_size: Vec2
    visible: bool
    opacity: float  # range:0,1
    clip_contents: bool

    def __init__(self) -> None: ...
    def grab_focus(self) -> None: ...
    def release_focus(self) -> None: ...
    def has_focus(self) -> bool: ...
    def is_hovered(self) -> bool: ...
    # signals: mouse_entered, mouse_exited, resized

class Material(Resource):
    albedo: Color
    metallic: float  # range:0,1
    roughness: float  # range:0,1
    emissive: Color
    emissive_strength: float  # range:0,32
    normal_scale: float  # range:0,4
    occlusion_strength: float  # range:0,1
    uv_scale: Vec2
    uv_offset: Vec2
    alpha_cutoff: float  # range:0,1
    unlit: bool
    double_sided: bool
    cast_shadows: bool
    shader: str

    def __init__(self) -> None: ...
    def touch(self) -> None: ...

class Mesh(Resource):
    bounds: AABB
    vertex_count: int
    triangle_count: int
    uploaded: bool

    def __init__(self) -> None: ...
    def clear(self) -> None: ...
    def compute_normals(self, smooth_angle: float = 1.0472) -> None: ...
    def compute_tangents(self) -> None: ...
    def compute_bounds(self) -> None: ...
    def weld(self, epsilon: float = 1e-05) -> int: ...
    def flip_winding(self) -> None: ...
    def transform(self, transform: Transform3D) -> None: ...

class NetSync(Node):
    net_id: int
    spawn_class: str
    interpolate: bool

    def __init__(self) -> None: ...
    def add_property(self, name: str) -> None: ...

class Node3D(Node):
    transform: Transform3D
    global_transform: Transform3D
    position: Vec3
    global_position: Vec3
    basis: Basis
    rotation: Quat
    euler: Vec3  # radians, packed as (yaw, pitch, roll)
    scale: float
    visible: bool
    layers: int

    def __init__(self) -> None: ...
    def translate(self, delta: Vec3) -> None: ...
    def rotate(self, axis: Vec3, angle: float) -> None: ...
    def look_at(self, target: Vec3, up: Vec3 = ...) -> None: ...
    def forward(self) -> Vec3: ...
    def up(self) -> Vec3: ...
    def right(self) -> Vec3: ...
    def is_visible_in_tree(self) -> bool: ...

class PackedScene(Resource):
    path: str

    def __init__(self) -> None: ...
    def instantiate(self) -> Node | None: ...
    def is_valid(self) -> bool: ...

class Texture(Resource):
    width: int
    height: int
    path: str
    valid: bool

class AudioListener3D(Node3D):
    def __init__(self) -> None: ...
    def make_current(self) -> None: ...
    def is_current(self) -> bool: ...

class AudioPlayer3D(Node3D):
    volume: float  # range:0,4
    pitch: float  # range:0.25,4
    loop: bool
    autoplay: bool
    max_distance: float  # range:1,500
    reference_distance: float  # range:0.1,20
    panning: float  # range:0,1
    doppler: float  # range:0,4
    portal_hops: int  # range:0,3
    portal_transmission: float  # range:0,1

    def __init__(self) -> None: ...
    def play(self) -> None: ...
    def stop(self) -> None: ...
    def is_playing(self) -> bool: ...
    def playback_position(self) -> float: ...
    def effective_distance(self) -> float: ...
    def effective_direction(self) -> Vec3: ...
    def portals_used(self) -> int: ...

class Button(Control):
    text: str
    toggle_mode: bool
    pressed: bool
    disabled: bool
    def __init__(self) -> None: ...
    # signals: pressed, toggled

class Camera3D(Node3D):
    fov: float  # range:1,179
    near: float
    far: float
    ortho_height: float
    frustum_offset: Vec2
    cull_mask: int
    scale_clip_planes: bool
    mode: int
    current: bool

    def __init__(self) -> None: ...
    def make_current(self) -> None: ...
    def get_projection(self, a0: float) -> Projection: ...
    def set_custom_projection(self, projection: Projection) -> None: ...
    def get_view_matrix(self) -> Transform3D: ...
    def project_point(self, world: Vec3, aspect: float) -> Vec3: ...

class CharacterBody3D(Node3D):
    radius: float
    height: float
    velocity: Vec3
    gravity: float
    step_height: float
    max_slope: float
    collision_mask: int
    min_size: float
    max_size: float
    size: float

    def __init__(self) -> None: ...
    def move_and_slide(self, world: PhysicsWorld | None, dt: float) -> bool: ...
    def is_on_floor(self) -> bool: ...
    def is_on_wall(self) -> bool: ...
    def is_on_ceiling(self) -> bool: ...
    def get_floor_normal(self) -> Vec3: ...
    def world_radius(self) -> float: ...
    def world_height(self) -> float: ...
    def eye_height(self) -> float: ...
    def scaled(self, value: float) -> float: ...
    def portals_traversed(self) -> int: ...
    # signals: portal_traversed

class ColorRect(Control):
    colour: Color
    def __init__(self) -> None: ...

class Container(Control):
    pass

class Label(Control):
    text: str
    use_theme_colour: bool
    colour: Color
    align: int  # range:0,2
    vertical_centre: bool
    def __init__(self) -> None: ...

class Light3D(Node3D):
    colour: Color
    energy: float  # range:0,64
    shadows: bool
    shadow_bias: float
    shadow_normal_bias: float

class LineEdit(Control):
    text: str
    placeholder: str
    secret: bool
    max_length: int
    def __init__(self) -> None: ...
    # signals: text_changed, text_submitted

class MeshInstance3D(Node3D):
    mesh: Mesh | None
    material: Material | None
    cast_shadows: bool
    receive_shadows: bool
    tint: Color

    def __init__(self) -> None: ...
    def set_material(self, slot: int, material: Material | None) -> None: ...
    def get_material(self, slot: int) -> Material | None: ...
    def get_material_count(self) -> int: ...
    def get_world_bounds(self) -> AABB: ...

class Panel(Control):
    use_theme_colour: bool
    colour: Color
    border: bool
    def __init__(self) -> None: ...

class Portal3D(Node3D):
    width: float  # range:0.05,64
    height: float  # range:0.05,64
    active: bool
    max_recursion: int  # range:0,8
    edge_colour: Color
    edge_width: float  # range:0,0.5
    open: float  # range:0,1
    link: Portal3D | None

    def __init__(self) -> None: ...
    def link_to(self, other: Portal3D | None) -> None: ...
    def unlink(self) -> None: ...
    def get_link(self) -> Portal3D | None: ...
    def is_linked(self) -> bool: ...
    def get_normal(self) -> Vec3: ...
    def get_plane(self) -> Plane: ...
    def get_bounds(self) -> AABB: ...
    def world_width(self) -> float: ...
    def world_height(self) -> float: ...
    def warp_out(self) -> Transform3D: ...
    def scale_out(self) -> float: ...
    def within_aperture(self, world_point: Vec3, margin: float = 0) -> bool: ...
    def closest_point(self, world_point: Vec3) -> Vec3: ...
    def faces(self, point: Vec3) -> bool: ...
    # signals: traversed

class ProgressBar(Control):
    value: float
    min_value: float
    max_value: float
    show_percentage: bool

    def __init__(self) -> None: ...
    def fraction(self) -> float: ...

class Slider(Control):
    min_value: float
    max_value: float
    step: float
    vertical: bool

    def __init__(self) -> None: ...
    def set_value(self, value: float) -> None: ...
    # signals: value_changed

class StaticBody3D(Node3D):
    def __init__(self) -> None: ...
    def build_from_mesh(self, world: PhysicsWorld | None, mesh: Mesh | None, layer: int = 1) -> None: ...
    def release(self) -> None: ...

class TextureRect(Control):
    modulate: Color
    def __init__(self) -> None: ...

class ThemeProvider(Control):
    def __init__(self) -> None: ...

class BoxContainer(Container):
    vertical: bool
    separation: float

class CenterContainer(Container):
    def __init__(self) -> None: ...

class CheckBox(Button):
    def __init__(self) -> None: ...

class DirectionalLight3D(Light3D):
    shadow_distance: float
    cascade_splits: Vec4
    cascade_count: int  # range:1,4

    def __init__(self) -> None: ...
    def direction(self) -> Vec3: ...

class GridContainer(Container):
    columns: int  # range:1,16
    separation: float
    def __init__(self) -> None: ...

class MarginContainer(Container):
    margin_left: float
    margin_top: float
    margin_right: float
    margin_bottom: float
    def __init__(self) -> None: ...

class OmniLight3D(Light3D):
    range: float
    attenuation: float
    radius: float
    def __init__(self) -> None: ...

class PanelContainer(Container):
    use_theme_colour: bool
    colour: Color
    def __init__(self) -> None: ...

class HBoxContainer(BoxContainer):
    def __init__(self) -> None: ...

class SpotLight3D(OmniLight3D):
    angle: float
    angle_softness: float
    def __init__(self) -> None: ...

class VBoxContainer(BoxContainer):
    def __init__(self) -> None: ...

# --------------------------------------------------------- the module

def log(message: str) -> None:
    """Write a line to the engine log."""

def warn(message: str) -> None:
    """Write a warning."""

def error(message: str) -> None:
    """Write an error."""

def root() -> Node | None:
    """The scene tree's root node."""

def scene() -> Node | None:
    """The current scene's root node."""

def physics() -> Object | None:
    """The physics world."""

def camera() -> Camera3D | None:
    """The active camera."""

def instantiate(class_name: str) -> Object | None:
    """Make an instance of an engine class by name."""

def classes() -> list[str]:
    """Every registered class name."""

def group(name: str) -> list[Node]:
    """Every node in a group."""

def key_down(key: int) -> bool:
    """Is a key held?"""

def action_down(action: str) -> bool:
    """Is an action held?"""

def action_pressed(action: str) -> bool:
    """Was an action pressed this frame?"""

def action_vector(neg_x: str, pos_x: str, neg_y: str, pos_y: str) -> Vec2:
    """A movement vector from four actions."""

def bind_action(action: str, key: int) -> None:
    """Bind a key to an action."""

def mouse_motion() -> Vec2:
    """Mouse movement since the last frame."""

def time() -> float:
    """Seconds since the scene started."""

def fps() -> float:
    """Smoothed frames per second."""

def frame() -> int:
    """The frame number."""
