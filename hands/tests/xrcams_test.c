/*
 * hands/camd/xrcams.c's camera maps, offline (DEE-311): which role a sensor's name gives
 * (role_for_sensor), where the image really sits in plane 0 (xr_camera_stride,
 * xr_camera_layout) for the cameras measured on the Frame, and xr_slugify. Pinned as they
 * behave now; no device, no XRService, nothing opened. xrcams.c is included to reach its
 * static role_for_sensor.
 *
 *   gcc -std=gnu11 -Wall -Wextra -Werror hands/tests/xrcams_test.c -o build/tests/xrcams-test
 */
#include "../camd/xrcams.c"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static xr_camera_t cam(unsigned w, unsigned h, uint32_t fmt, unsigned bpl, size_t plane0,
                       const char *sensor, const char *role)
{
    xr_camera_t c;
    memset(&c, 0, sizeof c);
    c.width = w, c.height = h, c.pixfmt = fmt, c.bytesperline = bpl, c.planesize[0] = plane0;
    snprintf(c.sensor, sizeof c.sensor, "%s", sensor);
    c.role = role;
    return c;
}

static void check_layout(const char *what, const xr_camera_t *c, xr_fmt_t fmt, unsigned pitch,
                         unsigned rows, unsigned width)
{
    xr_layout_t l;
    xr_camera_layout(c, &l);
    CHECK(l.fmt == fmt && l.pitch == pitch && l.rows == rows && l.width == width && l.height == c->height,
          "%s: got %s pitch %u rows %u width %u height %u", what, xr_fmt_name(l.fmt), l.pitch, l.rows,
          l.width, l.height);
}

int main(void)
{
    /* sensor -> role */
    CHECK(!strcmp(role_for_sensor("og01a1b 2-0060"), "tracking"), "side fisheye");
    CHECK(!strcmp(role_for_sensor("og0ve10 3-0010"), "tracking"), "upper");
    CHECK(!strcmp(role_for_sensor("arcimx616 5-001a"), "passthrough"), "Arcturus colour");
    CHECK(!strcmp(role_for_sensor("ov7251 eye"), "unknown"), "anything else");

    /* the side cameras through the ISP: NV12, 1056 wide, 1152 bytes a row; the luma is the image */
    xr_camera_t side = cam(1056, 1024, V4L2_PIX_FMT_NV12, 1056, (size_t)1152 * 1024 * 3 / 2, "og01a1b", "tracking");
    CHECK(xr_camera_stride(&side) == 1152, "side stride %u", xr_camera_stride(&side));
    check_layout("side, ISP", &side, XR_FMT_GREY8, 1152, 1024, 1056);
    /* ...with a plane 0 that holds the luma alone */
    side.planesize[0] = (size_t)1152 * 1024;
    CHECK(xr_camera_stride(&side) == 1152, "luma-only stride %u", xr_camera_stride(&side));

    /* the side cameras without the ISP: grey, rows as V4L2 says */
    xr_camera_t grey = cam(640, 480, V4L2_PIX_FMT_GREY, 640, 0, "og0ve10", "tracking");
    CHECK(xr_camera_stride(&grey) == 640, "grey stride %u", xr_camera_stride(&grey));
    check_layout("upper, grey", &grey, XR_FMT_GREY8, 640, 480, 640);
    grey.bytesperline = 0;
    CHECK(xr_camera_stride(&grey) == 640, "no bytesperline: the width");
    /* a plane 0 size that makes no sense: back to bytesperline */
    grey = cam(640, 480, V4L2_PIX_FMT_GREY, 704, 100, "og0ve10", "tracking");
    CHECK(xr_camera_stride(&grey) == 704, "nonsense plane size: stride %u", xr_camera_stride(&grey));

    /* the Arcturus colour camera: 10-bit packed YUV 4:2:0, 3080 bytes padded to 3328, 1972 valid */
    xr_camera_t colour = cam(2464, 2464, V4L2_PIX_FMT_NV12, 2464, (size_t)3328 * 3696, "arcimx616", "passthrough");
    check_layout("Arcturus", &colour, XR_FMT_YUV420_10P, 3328, 3696, 1972);

    /* NV12 that isn't a tracking camera: luma and chroma */
    xr_camera_t nv12 = cam(1280, 720, V4L2_PIX_FMT_NV12, 1280, (size_t)1280 * 720 * 3 / 2, "other", "unknown");
    check_layout("NV12", &nv12, XR_FMT_NV12, 1280, 1080, 1280);

    CHECK(!strcmp(xr_fmt_name(XR_FMT_GREY8), "grey8") && !strcmp(xr_fmt_name(XR_FMT_NV12), "nv12") &&
          !strcmp(xr_fmt_name(XR_FMT_YUV420_10P), "yuv420_10p") && !strcmp(xr_fmt_name((xr_fmt_t)99), "?"),
          "format names");

    char out[8];
    xr_slugify("og01a1b 2/0060", out, sizeof out);
    CHECK(!strcmp(out, "og01a1b"), "slug cut to the buffer: %s", out);
    char big[32];
    xr_slugify("og01a1b 2/0060", big, sizeof big);
    CHECK(!strcmp(big, "og01a1b_2_0060"), "slug: %s", big);

    printf("xrcams_test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures != 0;
}
