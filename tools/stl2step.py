#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
stl2step.py — STL(삼각 메시) → STEP(AP214 솔리드) 변환기

왜 필요한가
-----------
3D 프린팅용 STL은 삼각형 껍데기일 뿐입니다. Fusion 360·SolidWorks·NX 같은
CAD에서 치수를 재거나 구멍을 추가하려면 "솔리드"가 필요합니다.
이 스크립트는 메시를 닫힌 솔리드로 바꿔 STEP으로 내보냅니다.

까다로운 점 셋
--------------
1) **삼각형이 너무 많습니다.** 수십만 면을 그대로 STEP에 넣으면 파일이
   수백 MB가 되고 CAD가 열지 못합니다. → 품질별로 면 수를 줄입니다(decimate).

2) **한 파일에 여러 덩어리가 들어 있습니다.** (좌·우 선체가 한 STL에)
   전체를 한꺼번에 줄이면 덩어리 사이에 비매니폴드 모서리가 생깁니다.
   → 연결 성분으로 쪼개서 각각 줄입니다.

3) **속이 빈 부품은 껍데기가 둘입니다.** (바깥 껍데기 + 안쪽 공동)
   그대로 내보내면 안쪽 껍데기가 "또 하나의 양수 솔리드"가 되어
   부피가 엉뚱하게 커집니다. → 공동을 감싸는 껍데기를 찾아 void로 넣습니다.

사용법
------
    python tools/stl2step.py --in cad/stl --out cad/step
    python tools/stl2step.py --in cad/stl --out cad/step --quality fine
    python tools/stl2step.py --in part.stl --out ./ --quality coarse

    --quality coarse : 면 수 ~2.5천.  약 6 MB.  형상 확인용
               medium: 면 수 ~6천.    약 15 MB. 권장 — 치수 측정에 충분 (기본값)
               fine  : 면 수 ~20천.   약 48 MB. 곡면이 매끄러움
               none  : 줄이지 않음.   원본 그대로 (수백 MB가 될 수 있습니다)

파일 크기 주의
--------------
삼각형을 평면 면으로 그대로 옮기므로 **삼각형 1개당 약 2.4 KB**가 듭니다.
원본 20만 면을 그대로 내보내면 STEP이 500 MB에 달해 CAD가 열지 못합니다.
`medium` 이 기본값인 이유입니다. 더 매끄러운 곡면이 필요하면 `fine` 을 쓰되
파일 크기를 먼저 확인하세요.

필요 패키지
-----------
    pip install cadquery-ocp trimesh fast-simplification numpy
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np

# ─────────────────────────────────────────────────────────────
# 품질 설정 — 목표 삼각형 수
# ─────────────────────────────────────────────────────────────
QUALITY = {
    "coarse": 2_500,
    "medium": 6_000,
    "fine": 20_000,
    "none": None,
}

# STEP 파일에서 삼각 면 하나가 차지하는 대략적인 크기 (바이트)
BYTES_PER_FACE = 2_400

TOL = 1e-7  # OpenCASCADE 꼭짓점 병합 허용오차 (mm)


# ─────────────────────────────────────────────────────────────
# 1단계 · 메시 불러오기와 단순화
# ─────────────────────────────────────────────────────────────
def load_bodies(path: Path):
    """STL을 읽어 연결된 덩어리(body) 목록으로 돌려줍니다."""
    import trimesh

    mesh = trimesh.load_mesh(str(path), process=False)
    if isinstance(mesh, trimesh.Scene):
        mesh = trimesh.util.concatenate(tuple(mesh.geometry.values()))

    # STL은 꼭짓점이 삼각형마다 중복 저장되므로 한 번은 병합해야
    # 연결 성분을 찾을 수 있습니다.
    mesh.merge_vertices()

    bodies = mesh.split(only_watertight=False)
    if len(bodies) == 0:
        bodies = [mesh]
    return list(bodies)


def simplify(body, target: int | None):
    """
    덩어리 하나를 목표 면 수까지 줄입니다.

    주의 — 줄인 뒤에 다시 merge_vertices()나 fill_holes()를 호출하면
    안 됩니다. 이미 닫힌 메시에 오히려 경계를 만들어 버립니다.
    (실제로 겪은 문제입니다.)
    """
    import trimesh
    import fast_simplification

    nf = len(body.faces)
    if target is None or nf <= target:
        return body

    ratio = 1.0 - (target / nf)
    ratio = float(np.clip(ratio, 0.0, 0.97))

    v, f = fast_simplification.simplify(
        body.vertices.astype(np.float64),
        body.faces.astype(np.int32),
        ratio,
    )
    out = trimesh.Trimesh(vertices=v, faces=f, process=False)

    # 줄이는 과정에서 닫힘이 깨졌다면 덜 줄여서 다시 시도합니다.
    if not out.is_watertight and ratio > 0.3:
        v, f = fast_simplification.simplify(
            body.vertices.astype(np.float64),
            body.faces.astype(np.int32),
            ratio * 0.6,
        )
        retry = trimesh.Trimesh(vertices=v, faces=f, process=False)
        if retry.is_watertight:
            return retry
        return body  # 그래도 안 되면 원본을 씁니다

    return out


# ─────────────────────────────────────────────────────────────
# 2단계 · 메시 → OpenCASCADE 껍데기(Shell)
# ─────────────────────────────────────────────────────────────
def mesh_to_shell(body):
    """
    삼각형 하나를 평면 삼각 면으로 만들어 껍데기로 엮습니다.

    핵심은 **꼭짓점과 모서리를 공유**하는 것입니다. 삼각형마다 꼭짓점을
    새로 만들면 면들이 서로 붙지 않아 닫힌 껍데기가 되지 않습니다.
    """
    from OCP.BRep import BRep_Builder
    from OCP.BRepBuilderAPI import (
        BRepBuilderAPI_MakeEdge,
        BRepBuilderAPI_MakeFace,
        BRepBuilderAPI_MakeVertex,
        BRepBuilderAPI_MakeWire,
    )
    from OCP.gp import gp_Pnt
    from OCP.TopoDS import TopoDS, TopoDS_Shell

    # OCP 버전에 따라 이름이 TopoDS.Edge_s 또는 TopoDS.Edge 입니다.
    as_edge = getattr(TopoDS, "Edge_s", None) or TopoDS.Edge

    verts = body.vertices
    faces = body.faces

    # 꼭짓점을 미리 전부 만들어 둡니다.
    occ_verts = [
        BRepBuilderAPI_MakeVertex(gp_Pnt(float(p[0]), float(p[1]), float(p[2]))).Vertex()
        for p in verts
    ]

    edge_cache: dict[tuple[int, int], object] = {}

    def get_edge(a: int, b: int):
        """(a,b) 모서리를 돌려줍니다. 반대 방향이면 뒤집어 돌려줍니다."""
        key = (a, b) if a < b else (b, a)
        e = edge_cache.get(key)
        if e is None:
            e = BRepBuilderAPI_MakeEdge(occ_verts[key[0]], occ_verts[key[1]]).Edge()
            edge_cache[key] = e
        if (a, b) == key:
            return e
        # ⚠ Reversed()는 TopoDS_Shape를 돌려줍니다. Edge로 되돌려야
        #   MakeWire가 받아들입니다. (이걸 놓쳐서 면이 0개가 되는 일이 있었습니다.)
        return as_edge(e.Reversed())

    builder = BRep_Builder()
    shell = TopoDS_Shell()
    builder.MakeShell(shell)

    made = 0
    first_error: str | None = None
    for tri in faces:
        i, j, k = int(tri[0]), int(tri[1]), int(tri[2])
        if i == j or j == k or k == i:
            continue
        try:
            mw = BRepBuilderAPI_MakeWire()
            mw.Add(get_edge(i, j))
            mw.Add(get_edge(j, k))
            mw.Add(get_edge(k, i))
            if not mw.IsDone():
                continue
            mf = BRepBuilderAPI_MakeFace(mw.Wire(), True)  # True = 평면 가정
            if not mf.IsDone():
                continue
            builder.Add(shell, mf.Face())
            made += 1
        except Exception as exc:  # noqa: BLE001
            # 삼각형 하나가 실패하는 것은 흔한 일이라 건너뜁니다.
            # 다만 **전부** 실패하면 환경 문제이므로 첫 원인을 남겨 둡니다.
            if first_error is None:
                first_error = f"{type(exc).__name__}: {exc}"
            continue

    if made == 0 and first_error:
        raise RuntimeError(f"면을 하나도 만들지 못했습니다 — {first_error}")

    return shell, made


# ─────────────────────────────────────────────────────────────
# 3단계 · 공동(cavity) 찾기 — 속 빈 부품 처리
# ─────────────────────────────────────────────────────────────
def nest(bodies):
    """
    덩어리들을 (바깥 껍데기, [그 안의 공동들]) 묶음으로 정리합니다.

    판정 방법: 어떤 덩어리의 경계상자가 다른 덩어리의 경계상자 안에
    완전히 들어가면 공동으로 봅니다. 여러 겹이면 가장 가까운(가장 작은)
    바깥 껍데기에 붙입니다.
    """
    n = len(bodies)
    boxes = [b.bounds for b in bodies]
    vols = [float(abs(b.volume)) for b in bodies]

    def inside(a: int, b: int) -> bool:
        """a의 경계상자가 b의 경계상자 안에 들어가는가"""
        lo_a, hi_a = boxes[a]
        lo_b, hi_b = boxes[b]
        m = 1e-6
        return bool(np.all(lo_a >= lo_b - m) and np.all(hi_a <= hi_b + m) and vols[a] < vols[b])

    parent = [-1] * n
    for a in range(n):
        best = -1
        for b in range(n):
            if a != b and inside(a, b):
                if best == -1 or vols[b] < vols[best]:
                    best = b
        parent[a] = best

    groups = []
    for a in range(n):
        if parent[a] == -1:
            cavities = [c for c in range(n) if parent[c] == a]
            groups.append((a, cavities))
    return groups


# ─────────────────────────────────────────────────────────────
# 4단계 · 솔리드 조립과 STEP 내보내기
# ─────────────────────────────────────────────────────────────
def build_solid(outer_shell, cavity_shells):
    """바깥 껍데기로 솔리드를 만들고, 공동 껍데기를 void로 넣습니다."""
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeSolid

    ms = BRepBuilderAPI_MakeSolid()
    ms.Add(outer_shell)
    for sh in cavity_shells:
        ms.Add(sh)  # ← 안쪽 껍데기는 "구멍"으로 들어갑니다
    return ms.Solid()


def export_step(shapes, out_path: Path, name: str) -> bool:
    """여러 솔리드를 하나의 STEP 파일(AP214)로 내보냅니다."""
    from OCP.BRep import BRep_Builder
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.Interface import Interface_Static
    from OCP.STEPControl import STEPControl_AsIs, STEPControl_Writer
    from OCP.TopoDS import TopoDS_Compound

    builder = BRep_Builder()
    comp = TopoDS_Compound()
    builder.MakeCompound(comp)
    for s in shapes:
        builder.Add(comp, s)

    Interface_Static.SetCVal_s("write.step.schema", "AP214IS")
    Interface_Static.SetCVal_s("write.step.product.name", name)
    Interface_Static.SetCVal_s("write.step.unit", "MM")

    writer = STEPControl_Writer()
    writer.Transfer(comp, STEPControl_AsIs)
    status = writer.Write(str(out_path))
    return status == IFSelect_RetDone


# ─────────────────────────────────────────────────────────────
# 검증 · 부피 비교
# ─────────────────────────────────────────────────────────────
def solid_volume(shape) -> float:
    from OCP.BRepGProp import BRepGProp
    from OCP.GProp import GProp_GProps

    props = GProp_GProps()
    BRepGProp.VolumeProperties_s(shape, props)
    return abs(props.Mass())


# ─────────────────────────────────────────────────────────────
# 한 파일 변환
# ─────────────────────────────────────────────────────────────
def convert(src: Path, dst_dir: Path, target: int | None, verbose: bool = True) -> dict:
    t0 = time.time()
    name = src.stem
    result = {"file": src.name, "ok": False}

    bodies = load_bodies(src)
    raw_faces = sum(len(b.faces) for b in bodies)

    bodies = [simplify(b, target) for b in bodies]
    kept_faces = sum(len(b.faces) for b in bodies)

    groups = nest(bodies)

    # 기준 부피는 **공동을 뺀** 값이어야 합니다.
    # 단순히 |부피|를 모두 더하면 속 빈 부품에서 공동이 두 번 더해져
    # 멀쩡한 변환 결과가 오차 20 %로 보입니다.
    mesh_vol = 0.0
    for outer, cavities in groups:
        mesh_vol += abs(bodies[outer].volume)
        mesh_vol -= sum(abs(bodies[c].volume) for c in cavities)

    shells: dict[int, object] = {}
    total_made = 0
    for idx, body in enumerate(bodies):
        sh, made = mesh_to_shell(body)
        shells[idx] = sh
        total_made += made

    solids = []
    closed = 0
    for outer, cavities in groups:
        try:
            sol = build_solid(shells[outer], [shells[c] for c in cavities])
            solids.append(sol)
            closed += 1
        except Exception as exc:  # noqa: BLE001
            if verbose:
                print(f"    ! 솔리드 생성 실패(덩어리 {outer}): {exc}")

    if not solids:
        result["error"] = "솔리드를 하나도 만들지 못했습니다"
        return result

    dst_dir.mkdir(parents=True, exist_ok=True)
    out = dst_dir / f"{name}.step"
    ok = export_step(solids, out, name)

    step_vol = sum(solid_volume(s) for s in solids)
    err = abs(step_vol - mesh_vol) / mesh_vol * 100 if mesh_vol > 0 else 0.0

    result.update(
        ok=ok,
        out=str(out),
        bodies=len(bodies),
        solids=len(solids),
        faces_in=raw_faces,
        faces_out=kept_faces,
        mesh_cm3=mesh_vol / 1000.0,
        step_cm3=step_vol / 1000.0,
        vol_err_pct=err,
        mb=out.stat().st_size / 1e6 if out.exists() else 0.0,
        sec=time.time() - t0,
    )

    if verbose:
        flag = "✓" if ok and err < 1.0 else "△"
        print(
            f"  {flag} {src.name:<28} "
            f"덩어리 {len(bodies)}  면 {raw_faces:,}→{kept_faces:,}  "
            f"부피 {mesh_vol/1000:.1f}→{step_vol/1000:.1f} cm³ "
            f"(오차 {err:.3f}%)  {result['mb']:.1f} MB  {result['sec']:.1f}s"
        )
    return result


# ─────────────────────────────────────────────────────────────
def main() -> int:
    ap = argparse.ArgumentParser(
        description="STL → STEP 변환 (닫힌 솔리드, 공동 보존)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--in", dest="src", required=True, help="STL 파일 또는 폴더")
    ap.add_argument("--out", dest="dst", default="cad/step", help="출력 폴더")
    ap.add_argument(
        "--quality",
        default="medium",
        choices=list(QUALITY.keys()),
        help="coarse / medium(기본) / fine / none",
    )
    args = ap.parse_args()

    src = Path(args.src)
    dst = Path(args.dst)
    target = QUALITY[args.quality]

    if src.is_dir():
        files = sorted(src.glob("*.stl")) + sorted(src.glob("*.STL"))
    elif src.is_file():
        files = [src]
    else:
        print(f"경로를 찾을 수 없습니다: {src}", file=sys.stderr)
        return 1

    if not files:
        print(f"STL 파일이 없습니다: {src}", file=sys.stderr)
        return 1

    print(f"\nSTL → STEP 변환  ·  품질 {args.quality}"
          f"{f' (목표 {target:,} 면)' if target else ' (원본 유지)'}")
    print(f"입력 {len(files)}개  →  {dst}\n")

    results = [convert(f, dst, target) for f in files]

    ok = [r for r in results if r.get("ok")]
    bad = [r for r in results if not r.get("ok")]
    solids = sum(r.get("solids", 0) for r in ok)

    print(f"\n완료: {len(ok)}/{len(files)} 파일, 솔리드 {solids}개")
    if ok:
        worst = max(r["vol_err_pct"] for r in ok)
        print(f"최대 부피 오차: {worst:.3f}%")
    for r in bad:
        print(f"  ✗ {r['file']}: {r.get('error', '알 수 없는 오류')}")

    return 0 if not bad else 2


if __name__ == "__main__":
    sys.exit(main())
