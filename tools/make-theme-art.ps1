# make-theme-art.ps1 —— 把"带浅色背景的插画"抠成候选窗装饰图（透明 PNG，和 panda_bamboo.png 一样的用法）
#   * 从四边洪水填充去掉背景（只删与边框连通、且足够亮的像素；主体被轮廓围住，不会被吃掉）
#   * 丢掉小的孤立色块（草叶、小花之类）
#   * 边缘 1px 羽化，避免白边
#   * 按内容裁边、保持比例、缩放到目标方形尺寸
# 用法: powershell -ExecutionPolicy Bypass -File tools/make-theme-art.ps1 -Source a.jpg -Out b.png [-Size 256] [-Tol 40] [-MinIsland 600]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Size = 256,
    [int]$Tol = 40,
    [int]$MinIsland = 600
)
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class ThemeArt {
    public static string Process(string srcPath, string outPath, int outSize, int tol, int minIsland) {
        const int W = 700;
        using (var src = Image.FromFile(srcPath))
        using (var bmp = new Bitmap(W, W, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) {
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                g.Clear(Color.Transparent);
                g.DrawImage(src, 0, 0, W, W);
            }
            var rect = new Rectangle(0, 0, W, W);
            var data = bmp.LockBits(rect, ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
            int stride = data.Stride;
            var bytes = new byte[stride * W];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);

            Func<int,int,int> idx = (x, y) => y * stride + x * 4;
            Func<int,int,bool> bgLike = (x, y) => {
                int i = idx(x, y);
                int b = bytes[i], gg = bytes[i + 1], r = bytes[i + 2];
                return r >= 235 - tol && gg >= 235 - tol && b >= 215 - tol;
            };

            // 1) 从四边洪水填充
            var isBg = new bool[W * W];
            var seen = new bool[W * W];
            var stack = new Stack<int>();
            for (int x = 0; x < W; x++) { stack.Push(x); stack.Push((W - 1) * W + x); }
            for (int y = 0; y < W; y++) { stack.Push(y * W); stack.Push(y * W + W - 1); }
            while (stack.Count > 0) {
                int p = stack.Pop();
                if (seen[p]) continue;
                seen[p] = true;
                int py = p / W, px = p - py * W;
                if (!bgLike(px, py)) continue;
                isBg[p] = true;
                if (px > 0)     stack.Push(p - 1);
                if (px < W - 1) stack.Push(p + 1);
                if (py > 0)     stack.Push(p - W);
                if (py < W - 1) stack.Push(p + W);
            }

            // 2) 连通块：只保留够大的（主体/蝴蝶），丢小碎片
            var comp = new int[W * W];
            var keep = new bool[W * W];
            int nextComp = 0, keptComps = 0;
            var members = new List<int>();
            for (int p0 = 0; p0 < W * W; p0++) {
                if (isBg[p0] || comp[p0] != 0) continue;
                nextComp++;
                members.Clear();
                stack.Push(p0);
                comp[p0] = nextComp;
                while (stack.Count > 0) {
                    int q = stack.Pop();
                    members.Add(q);
                    int qy = q / W, qx = q - qy * W;
                    int[] nb = new int[4];
                    int n = 0;
                    if (qx > 0) nb[n++] = q - 1;
                    if (qx < W - 1) nb[n++] = q + 1;
                    if (qy > 0) nb[n++] = q - W;
                    if (qy < W - 1) nb[n++] = q + W;
                    for (int k = 0; k < n; k++) {
                        int r2 = nb[k];
                        if (isBg[r2] || comp[r2] != 0) continue;
                        comp[r2] = nextComp;
                        stack.Push(r2);
                    }
                }
                if (members.Count >= minIsland) {
                    keptComps++;
                    foreach (int m in members) keep[m] = true;
                }
            }

            // 3) alpha：保留=255，其余=0，再 3x3 均值羽化（只对保留像素计算，避免边缘变暗边）
            var alpha = new byte[W * W];
            for (int p = 0; p < W * W; p++) alpha[p] = keep[p] ? (byte)255 : (byte)0;
            var feather = new byte[W * W];
            for (int y = 0; y < W; y++) {
                for (int x = 0; x < W; x++) {
                    int p = y * W + x;
                    if (alpha[p] == 0) continue;
                    int sum = 0, cnt = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        int yy = y + dy; if (yy < 0 || yy >= W) continue;
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = x + dx; if (xx < 0 || xx >= W) continue;
                            sum += alpha[yy * W + xx]; cnt++;
                        }
                    }
                    feather[p] = (byte)(sum / Math.Max(cnt, 1));
                }
            }

            // 4) 写回 alpha + 求包围盒
            int minX = W, minY = W, maxX = -1, maxY = -1;
            for (int y = 0; y < W; y++) {
                for (int x = 0; x < W; x++) {
                    int p = y * W + x;
                    bytes[idx(x, y) + 3] = feather[p];
                    if (feather[p] > 8) {
                        if (x < minX) minX = x;
                        if (x > maxX) maxX = x;
                        if (y < minY) minY = y;
                        if (y > maxY) maxY = y;
                    }
                }
            }
            Marshal.Copy(bytes, 0, data.Scan0, bytes.Length);
            bmp.UnlockBits(data);
            if (maxX < 0) return "FAILED: nothing kept";

            // 5) 裁到内容（方形、留 6% 边）并缩放
            int cw = maxX - minX + 1, ch = maxY - minY + 1;
            int side = (int)(Math.Max(cw, ch) * 1.06);
            int cx0 = minX - (side - cw) / 2;
            int cy0 = minY - (side - ch) / 2;
            if (cx0 < 0) { side += cx0; cx0 = 0; }
            if (cy0 < 0) { side += cy0; cy0 = 0; }
            if (cx0 + side > W) side = W - cx0;
            if (cy0 + side > W) side = W - cy0;

            using (var final = new Bitmap(outSize, outSize, PixelFormat.Format32bppArgb)) {
                using (var g2 = Graphics.FromImage(final)) {
                    g2.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                    g2.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                    g2.Clear(Color.Transparent);
                    g2.DrawImage(bmp, new Rectangle(0, 0, outSize, outSize),
                                 new Rectangle(cx0, cy0, side, side), GraphicsUnit.Pixel);
                }
                final.Save(outPath, ImageFormat.Png);
            }
            return string.Format("ok: {0} (content {1}x{2}, kept {3} of {4} components)",
                                 outPath, cw, ch, keptComps, nextComp);
        }
    }
}
"@
$r = [ThemeArt]::Process((Resolve-Path $Source).Path, [IO.Path]::GetFullPath($Out), $Size, $Tol, $MinIsland)
Write-Host $r