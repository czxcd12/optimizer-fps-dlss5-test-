# NTE silhouette history rejection

The user reports persistent character/building shimmer after the sky and subpixel-history corrections.
Recorded frames alone cannot identify which runtime rejection path caused it. The latest recording was
inspected as extracted consecutive frames, not continuous playback.

Reference comparison: Janblade's `dlssnr_detail_reuse.hlsl` and `DlssNrDetailReuse.h` are byte-identical
between v0.1.30 and v0.1.32. The user remembers successful every-second-frame rendering with v0.1.32.
The reference uses neighbourhood colour statistics and two motion candidates; this addon uses a strict
centre-colour silhouette test and a binary 0.5 acceptance threshold. The comparison identifies a candidate
mechanism, not proof of the NTE cause. Reference source:
https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/blob/v0.1.32/OptiScaler/shaders/dlssnr/precompile/dlssnr_detail_reuse.hlsl

## Correction

For the centre tap on a silhouette only, collect linear-colour mean and variance from a 3x3 neighbourhood.
Exclude non-finite colours and depths that do not match the centre under the existing depth tolerance.
Compare historical colour against the mean +/- one standard deviation using the existing strict relative
colour thresholds and chromaticity/luminance gate. The stronger of this test and the original centre test
is used. At least three matching-depth samples are required. Motion selection, depth rejection, fill,
frame cadence and sky processing are unchanged. This is independently implemented for this addon's
existing resources and gates; no reference source was copied.

In the regression, centre intensity changes from 0.63 to 0.66 while history remains 0.5 and matching-depth
neighbours contain 0.4/0.8 variation. The old centre trust changes from approximately 0.510 to 0.108:
the carried residual abruptly drops from 0.125 to zero. The local test accepts both samples. A uniform
changed surface still rejects, as does a surface whose matching colour exists only across the depth edge.
Windows WARP tests cover both colour domains, both depth conventions, alpha preservation and the old
shader as a failing control. Existing fractional-motion tests continue to run in the same executable.

## Limits

The variance check adds up to nine colour/depth samples at silhouette centres. GPU cost has not been
measured. It can admit stale colour within a textured surface's local variation; depth rejection limits
that risk but cannot eliminate it. Wrong motion or an incorrect centre depth remain possible causes.
Do not label this a confirmed NTE fix until an in-game comparison succeeds.
