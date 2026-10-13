#!/usr/bin/env python3
# 另一个游戏的着色器出包：输入是宿主导出的着色器记录（DM_SHADER_DUMP 目录里的 <键>.rec，见 nvn/hle_nvn.c shader_dump），
# 先把它们整理成 dmsh_build.py 认的形态（每个着色器一个单记录的包文件 + 一份映射表），再原样跑 dmsh_build.py 的翻译链。
# 不改 dmsh_build.py（它的内容计入翻译链版本，改了就要重出暗黑 II 的全部着色器）：这里读它的源码、只把映射表路径一行换掉后执行。
# 阶段取自代码区里着色器程序头（代码区 +0x30 的第一个字，第 10–13 位）：1 = 顶点，5 = 像素；其余（几何等）Metal 无对应，跳过并列出。
# 作答表（nvn/shader_facts.tsv）里没有这些键：贴图一律按 2D、顶点属性与片元输出按浮点翻译，用到立方体 / 3D / 整型的着色器要另补作答。
# 用法：dmsh_dump_build.py <导出目录> <工作目录>      环境 J=并发
# 产物：<工作目录>/map/shader_map.tsv（宿主用 DM_NVN_SHMAP 指它）、<工作目录>/lib/b-<链版本>/<键>_b.metallib 等（宿主用 DM_MSL_DIR=<工作目录>/lib）
# Shader build for another game: the input is the shader records dumped by the host (<key>.rec in the DM_SHADER_DUMP directory, see shader_dump in nvn/hle_nvn.c);
# they are first arranged into the form dmsh_build.py understands (one single-record package file per shader + a mapping table), and then dmsh_build.py's translation chain is run as is.
# dmsh_build.py is not modified (its content counts toward the translation chain version; changing it would require rebuilding all of Diablo II's shaders): its source is read here and executed with only the mapping-table path line replaced.
# The stage comes from the shader program header in the code area (first word at code +0x30, bits 10-13): 1 = vertex, 5 = pixel; others (geometry etc.) have no Metal counterpart and are skipped and listed.
# The answer table (nvn/shader_facts.tsv) has none of these keys: textures are all translated as 2D and vertex attributes / fragment outputs as float; shaders using cube / 3D / integer types need answers added separately.
# Usage: dmsh_dump_build.py <dump dir> <work dir>      environment J=concurrency
# Output: <work dir>/map/shader_map.tsv (point the host at it with DM_NVN_SHMAP), <work dir>/lib/b-<chain version>/<key>_b.metallib etc. (host: DM_MSL_DIR=<work dir>/lib)
import os, sys, struct, hashlib

dump, work = sys.argv[1:3]
HERE = os.path.dirname(os.path.abspath(__file__))
pkg, mapdir, lib = (os.path.join(work, d) for d in ('pkg', 'map', 'lib'))
for d in (pkg, mapdir, lib): os.makedirs(d, exist_ok=True)
STAGE = {1: ('vs', '顶点'), 5: ('ps', '像素')}
rows, skipped = [], []
for n in sorted(os.listdir(dump)):
    if not n.endswith('.rec'): continue
    rec = open(os.path.join(dump, n), 'rb').read(); key = n[:-4]
    ci = rec.find(bytes.fromhex('34127698'))
    if ci < 0x38 + 0x34 or hashlib.sha256(rec[ci:ci + 0x880]).hexdigest()[:16] != key: sys.exit(f'【红】{n}：记录布局或键不符')
    st = (struct.unpack_from('<I', rec, 0x38 + 0x30)[0] >> 10) & 0xf
    if st not in STAGE: skipped.append((key, st)); continue
    name = f'{STAGE[st][0]}_{key}.bin'
    open(os.path.join(pkg, name), 'wb').write(b'BTCDBTCD' + rec[8:])
    rows.append(f'{name}\t0\t{STAGE[st][1]}\t{key}\t{"0" * 16}\t1\t-1\t-1\t-\t-\t-\t-\t0\n')
MAP = os.path.join(mapdir, 'shader_map.tsv')
open(MAP, 'w').write('# kind: evidence  宿主导出的着色器 → 映射表（dm/gfx/tools/dmsh_dump_build.py）\npackage\tindex\tstage\tdm_ctl_sha16\tpc_dxbc_sha16\tmetallib\n' + ''.join(rows))
open(MAP + '.conflicts.tsv', 'w').write('# 无\n')
open(os.path.join(work, 'skipped.txt'), 'w').write(''.join(f'{k}\t阶段号 {s}\n' for k, s in skipped))   # 跳过的键（bringup.sh 据此不把它们算作「新记录」） | skipped keys (bringup.sh uses this so they do not count as "new records")
print(f'着色器记录 {len(rows) + len(skipped)}：顶点 {sum("顶点" in r for r in rows)}，像素 {sum("像素" in r for r in rows)}，跳过 {len(skipped)}（阶段号 {sorted({s for _, s in skipped})}）')

build = os.path.join(HERE, 'dmsh_build.py'); src = open(build).read()
old = "MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../nvn/shader_map.tsv')"
if src.count(old) != 1: sys.exit('【红】dmsh_build.py 里找不到映射表路径那一行（它改过了，这里要跟着改）')
src = src.replace(old, f'MAP = {MAP!r}')

# 片元输出的非数（NaN）一律换成 0：有的着色器会给隐藏的界面元素算出 NaN（顶点色为 0、贴图坐标是没初始化的值，无穷 × 0），
# 图形处理器对「NaN 写进定点颜色目标」各有各的结果——Switch 上是 0，苹果的 GPU 上是不确定的值（实测成品红色方块）。
# 做法：spirv-cross 出的 MSL 在编译前，给主函数每个 return out 前面补一句逐个浮点颜色输出的 select。同样只换执行时的源码，不改 dmsh_build.py。
# 缓存目录带上这条规则的版本号：规则变了就不会命中旧缓存。
# Not-a-number (NaN) fragment outputs are replaced with 0: some shaders compute NaN for hidden UI elements (vertex colour 0, texture coordinates left uninitialized, infinity x 0),
# and GPUs differ on "NaN written to a fixed-point colour target" -- 0 on the Switch, an undefined value on Apple GPUs (observed as magenta rectangles).
# How: before the MSL from spirv-cross is compiled, a select over each float colour output is added in front of every return out of the main function. Again only the executed source is changed, not dmsh_build.py.
# The cache directory carries this rule's version number: when the rule changes, old cache entries no longer hit.
NAN_GUARD_VERSION = 3   # 3：加透明度测试的两个函数常量（200 比较函数、201 参考值；回放层 nvn_mtl.m at_fn 按它们特化） | 3: adds the two alpha test function constants (200 compare function, 201 reference value; the replay layer's at_fn in nvn_mtl.m specializes by them)
_OLD_NAN_GUARD_VERSION = 2   # 2：无穷也收住（钳到半精度浮点的最大值）——无穷输出在混合阶段乘以 0 的混合系数同样会变成 NaN | 2: infinities are contained too (clamped to the half-float maximum) -- an infinite output multiplied by a blend factor of 0 in the blend stage also becomes NaN
def _nan_guard(msl, stage):
    if stage != 'ps': return
    import re
    t = open(msl).read()
    m = re.search(r'struct main0_out\s*\{(.*?)\};', t, re.S)
    outs = re.findall(r'\b(float[234]?)\s+(\w+)\s*\[\[color\(\d+\)', m.group(1)) if m else []
    if not outs or '    return out;\n' not in t: return
    guard = ''.join(f'    out.{n} = clamp(select(out.{n}, {ty}(0.0), isnan(out.{n})), {ty}(-65504.0), {ty}(65504.0));\n' for ty, n in outs)
    # 透明度测试：比较 0 号颜色输出的 alpha（NVN 比较函数 1 永不过 … 5 大于 … 8 永远过）；常量没给（dm_at_func = 0）时这段被编译器整段去掉
    # Alpha test: compares the alpha of colour output 0 (NVN compare functions 1 never ... 5 greater ... 8 always); when the constants are not given (dm_at_func = 0) the compiler removes this whole block
    c0 = re.search(r'\bfloat4\s+(\w+)\s*\[\[color\(0\)', m.group(1))
    if c0:
        a = f'out.{c0.group(1)}.w'
        guard += ('    if (dm_at_func != 0) { bool dm_p = dm_at_func == 1 ? false : dm_at_func == 2 ? ' + a + ' < dm_at_ref : dm_at_func == 3 ? ' + a + ' == dm_at_ref : dm_at_func == 4 ? ' + a + ' <= dm_at_ref : '
                  'dm_at_func == 5 ? ' + a + ' > dm_at_ref : dm_at_func == 6 ? ' + a + ' != dm_at_ref : dm_at_func == 7 ? ' + a + ' >= dm_at_ref : true; if (!dm_p) discard_fragment(); }\n')
        t = t.replace('using namespace metal;\n', 'using namespace metal;\n\nconstant int dm_at_func_c [[function_constant(200)]];\nconstant float dm_at_ref_c [[function_constant(201)]];\n'
                      'constant int dm_at_func = is_function_constant_defined(dm_at_func_c) ? dm_at_func_c : 0;\nconstant float dm_at_ref = is_function_constant_defined(dm_at_ref_c) ? dm_at_ref_c : 0.0;\n', 1)
    open(msl, 'w').write(t.replace('    return out;\n', guard + '    return out;\n'))
# 几何着色器 2b9a4dcc42474bb0（点扩四边形，见 nvn/nvn_mtl.m gs_quad_on 处的说明）的等效：给每个「输出恰好有 0–3 号四个 float4」的顶点着色器另出一个 <键>_gq 变体。
# 变体把原入口改名成普通函数，新入口先调它，再按顶点序号展开成四边形的一个角，并把输出改成片元阶段要的样子（0 号 = 颜色，1 号 = 贴图坐标）。
# 反射 json 照抄（绑定完全相同），只改键名。配对关系在出包时不知道（导出的记录里没有程序信息），所以符合形态的顶点着色器都出；用不上的不会被载入。
# Equivalent of geometry shader 2b9a4dcc42474bb0 (point to quad, see the notes at gs_quad_on in nvn/nvn_mtl.m): every vertex shader whose outputs are exactly the four float4s 0-3 additionally gets a <key>_gq variant.
# The variant renames the original entry to a plain function; the new entry calls it, expands to one corner of the quad by vertex index, and reshapes the outputs to what the fragment stage wants (0 = colour, 1 = texture coordinates).
# The reflection json is copied (the bindings are identical) with only the key changed. Pairing is unknown at build time (dumped records carry no program information), so every vertex shader of this shape gets one; unused ones are never loaded.
def _gq_variants(libdir):
    import re, glob, json, subprocess
    n = 0
    for f in sorted(glob.glob(os.path.join(libdir, '*_b.metal'))):
        t = open(f).read(); key = os.path.basename(f)[:-len('_b.metal')]
        sig = 'vertex main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])'
        m = re.search(r'struct main0_out\s*\{(.*?)\};', t, re.S)
        if sig not in t or not m: continue
        loc = {int(i): nm for ty, nm, i in re.findall(r'\b(float4)\s+(\w+)\s*\[\[user\(locn(\d+)\)\]\]', m.group(1))}
        if sorted(loc) != [0, 1, 2, 3] or len(re.findall(r'user\(locn', m.group(1))) != 4: continue
        t = t.replace(sig, 'static main0_out main0_base(main0_in in, constant spvDescriptorSetBuffer0& spvDescriptorSet0)')
        t += f"""
vertex main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint dm_vid [[vertex_id]])
{{
    main0_out o = main0_base(in, spvDescriptorSet0);
    float4 P = o.{loc[0]}, C = o.{loc[1]}, A = o.{loc[2]}, R = o.{loc[3]};
    uint c = dm_vid & 3u;
    float2 off = c == 0u ? -A.xy : c == 1u ? -A.zw : c == 2u ? A.zw : A.xy;
    o.gl_Position = float4(P.xy + off, P.z, P.w);
    o.{loc[0]} = C;
    o.{loc[1]} = float4((c & 1u) != 0u ? R.z : R.x, (c & 2u) != 0u ? R.w : R.y, (c & 1u) != 0u ? 1.0 : -1.0, (c & 2u) != 0u ? 1.0 : -1.0);
    return o;
}}
"""
        base = os.path.join(libdir, key + '_gq'); open(base + '.metal', 'w').write(t)
        try:
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-w', '-c', base + '.metal', '-o', base + '.air'], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', base + '.air', '-o', base + '.metallib'], check=True)
        except subprocess.CalledProcessError as e:
            print(f'【红】_gq 变体编不出 {key}：{e}'); continue
        finally:
            if os.path.exists(base + '.air'): os.remove(base + '.air')
        j = json.load(open(os.path.join(libdir, key + '_b.json'))); j['key'] = key + '_gq'; json.dump(j, open(base + '.json', 'w'), ensure_ascii=False, indent=1)
        n += 1
    print(f'点扩四边形的顶点变体（_gq）：{n} 个')
    # iOS 版：同一份 _gq 源码按 iOS 的编译口径（dm/ios/tools/ios_metal.py）再编一份到 IOSOUT 的同名目录
    # iOS version: the same _gq source is compiled again with the iOS compile settings (dm/ios/tools/ios_metal.py) into the same-named directory under IOSOUT
    ios_root = os.environ.get('IOSOUT')
    if ios_root:
        sys.path.insert(0, os.path.join(HERE, '../../ios_bringup/tools')); import ios_metal
        idir = os.path.join(ios_root, os.path.basename(libdir.rstrip('/'))); m = 0
        for f in sorted(glob.glob(os.path.join(libdir, '*_gq.metal'))):
            key = os.path.basename(f)[:-len('.metal')]; dst = os.path.join(idir, key)
            if not os.path.isdir(idir): break
            try: ios_metal.compile(f, dst + '.air', dst + '.metallib')
            except subprocess.CalledProcessError as e: print(f'【红】iOS _gq 变体编不出 {key}：{e}'); continue
            finally:
                if os.path.exists(dst + '.air'): os.remove(dst + '.air')
            j = json.load(open(os.path.join(libdir, key + '.json'))); j['chain_metal'] = ios_metal.RECORD; json.dump(j, open(dst + '.json', 'w'), ensure_ascii=False, indent=1); m += 1
        print(f'点扩四边形的顶点变体（_gq，iOS）：{m} 个')
hook = "    src = open(msl).read()\n    try:"
if src.count(hook) != 1: sys.exit('【红】dmsh_build.py 的 finish() 里找不到读 MSL 的那一行（它改过了，这里要跟着改）')
src = src.replace(hook, "    _nan_guard(msl, j['stage'])\n" + hook)
os.environ.setdefault('DMSH_CAS', os.path.join(lib, f'cas-nanguard{NAN_GUARD_VERSION}'))
sys.argv = [build, pkg, lib]
import atexit, glob as _glob
atexit.register(lambda: [_gq_variants(d) for d in _glob.glob(os.path.join(lib, 'b-*'))])   # dmsh_build 跑完（它自己会以退出收尾）之后再出变体 | Build the variants after dmsh_build has finished (it ends by exiting on its own)
exec(compile(src, build, 'exec'), {'__file__': build, '__name__': '__main__', '_nan_guard': _nan_guard})
