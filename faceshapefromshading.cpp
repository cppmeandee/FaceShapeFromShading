#ifndef FACE_SHAPE_FROM_SHADING_H
#define FACE_SHAPE_FROM_SHADING_H

#include "Geometry/geometryutils.hpp"
#include "Utils/utility.hpp"

#include <QApplication>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOffscreenSurface>
#include <QFile>

#include <GL/freeglut_std.h>
#include <boost/timer/timer.hpp>

#include "common.h"
#include "glm/glm.hpp"

#include <MultilinearReconstruction/basicmesh.h>
#include <MultilinearReconstruction/ioutilities.h>
#include <MultilinearReconstruction/multilinearmodel.h>
#include <MultilinearReconstruction/parameters.h>

#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"

namespace fs = boost::filesystem;

struct PixelInfo {
  PixelInfo() : fidx(-1) {}
  PixelInfo(int fidx, glm::vec3 bcoords) : fidx(fidx), bcoords(bcoords) {}

  int fidx;           // trinagle index
  glm::vec3 bcoords;  // bary centric coordinates
};

struct ImageBundle {
  ImageBundle() {}
  ImageBundle(const QImage& image, const vector<Constraint2D>& points, const ReconstructionResult& params)
    : image(image), points(points), params(params) {}
  QImage image;
  vector<Constraint2D> points;
  ReconstructionResult params;
};

int main(int argc, char **argv) {
  QApplication a(argc, argv);
  glutInit(&argc, argv);

  //google::InitGoogleLogging(argv[0]);

  const string model_filename("/home/phg/Data/Multilinear/blendshape_core.tensor");
  const string id_prior_filename("/home/phg/Data/Multilinear/blendshape_u_0_aug.tensor");
  const string exp_prior_filename("/home/phg/Data/Multilinear/blendshape_u_1_aug.tensor");
  const string template_mesh_filename("/home/phg/Data/Multilinear/template.obj");
  const string contour_points_filename("/home/phg/Data/Multilinear/contourpoints.txt");
  const string landmarks_filename("/home/phg/Data/Multilinear/landmarks_73.txt");
  const string albedo_index_map_filename("/home/phg/Data/Multilinear/albedo_index.png");
  const string albedo_pixel_map_filename("/home/phg/Data/Multilinear/albedo_pixel.png");

  BasicMesh mesh(template_mesh_filename);
  auto landmarks = LoadIndices(landmarks_filename);
  auto contour_indices = LoadContourIndices(contour_points_filename);

  const int tex_size = 2048;

  auto encode_index = [](int idx, unsigned char& r, unsigned char& g, unsigned char& b) {
    r = static_cast<unsigned char>(idx & 0xff); idx >>= 8;
    g = static_cast<unsigned char>(idx & 0xff); idx >>= 8;
    b = static_cast<unsigned char>(idx & 0xff);
  };

  auto decode_index = [](unsigned char r, unsigned char g, unsigned char b, int& idx) {
    idx = b; idx <<= 8; idx |= g; idx <<= 8; idx |= r;
    return idx;
  };

  // Generate index map for albedo
  bool generate_index_map = false;
  QImage albedo_index_map;
  if(QFile::exists(albedo_index_map_filename.c_str()) && (!generate_index_map)) {
    PhGUtils::message("loading index map for albedo.");
    albedo_index_map = QImage(albedo_index_map_filename.c_str());
    albedo_index_map.save("albedo_index.png");
  } else {
    PhGUtils::message("generating index map for albedo.");
    boost::timer::auto_cpu_timer t("index map for albedo generation time = %w seconds.\n");
    QSurfaceFormat format;
    format.setMajorVersion(3);
    format.setMinorVersion(3);

    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();

    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create())
      qFatal("Cannot create the requested OpenGL context!");
    context.makeCurrent(&surface);

    const QRect drawRect(0, 0, tex_size, tex_size);
    const QSize drawRectSize = drawRect.size();

    QOpenGLFramebufferObjectFormat fboFormat;
    fboFormat.setSamples(16);
    fboFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);

    QOpenGLFramebufferObject fbo(drawRectSize, fboFormat);
    fbo.bind();

    // draw the triangles

    // setup OpenGL viewing
#define DEBUG_GEN 0   // Change this to 1 to generate albedo pixel map
#if DEBUG_GEN
    glShadeModel(GL_SMOOTH);
#else
    glShadeModel(GL_FLAT);
#endif
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0.0, 1.0, 0.0, 1.0);
    glViewport(0, 0, tex_size, tex_size);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    PhGUtils::message("rendering index map.");
    for(int face_i = 0; face_i < mesh.NumFaces(); ++face_i) {
      auto normal_i = mesh.normal(face_i);
      auto f = mesh.face_texture(face_i);
      auto t0 = mesh.texture_coords(f[0]), t1 = mesh.texture_coords(f[1]), t2 = mesh.texture_coords(f[2]);
      unsigned char r, g, b;
      encode_index(face_i, r, g, b);
      int tmp_idx;
      assert(decode_index(r, g, b, tmp_idx) == face_i);
      glBegin(GL_TRIANGLES);

#if DEBUG_GEN
      glColor3f(1, 0, 0);
      glVertex2f(t0[0], t0[1]);
      glColor3f(0, 1, 0);
      glVertex2f(t1[0], t1[1]);
      glColor3f(0, 0, 1);
      glVertex2f(t2[0], t2[1]);
#else
      glColor4ub(r, g, b, 255);
      glVertex2f(t0[0], t0[1]);
      glVertex2f(t1[0], t1[1]);
      glVertex2f(t2[0], t2[1]);
#endif
      glEnd();
    }
    PhGUtils::message("done.");

    // get the bitmap and save it as an image
    QImage img = fbo.toImage();
    fbo.release();
    img.save("albedo_index.png");
    albedo_index_map = img;
  }

  // Compute the barycentric coordinates for each pixel
  vector<vector<PixelInfo>> albedo_pixel_map(tex_size, vector<PixelInfo>(tex_size));

  // Generate pixel map for albedo
  bool gen_pixel_map = false;
  QImage pixel_map_image;
  if(QFile::exists(albedo_pixel_map_filename.c_str()) && (!gen_pixel_map)) {
    pixel_map_image = QImage(albedo_pixel_map_filename.c_str());

    PhGUtils::message("generating pixel map for albedo ...");
    boost::timer::auto_cpu_timer t("pixel map for albedo generation time = %w seconds.\n");

    for(int i=0;i<tex_size;++i) {
      for(int j=0;j<tex_size;++j) {
        QRgb pix = albedo_index_map.pixel(j, i);
        unsigned char r = static_cast<unsigned char>(qRed(pix));
        unsigned char g = static_cast<unsigned char>(qGreen(pix));
        unsigned char b = static_cast<unsigned char>(qBlue(pix));
        if(r == 0 && g == 0 && b == 0) continue;
        int fidx;
        decode_index(r, g, b, fidx);

        QRgb bcoords_pix = pixel_map_image.pixel(j, i);

        float x = static_cast<float>(qRed(bcoords_pix)) / 255.0f;
        float y = static_cast<float>(qGreen(bcoords_pix)) / 255.0f;
        float z = static_cast<float>(qBlue(bcoords_pix)) / 255.0f;
        albedo_pixel_map[i][j] = PixelInfo(fidx, glm::vec3(x, y, z));
      }
    }
    PhGUtils::message("done.");
  } else {
    pixel_map_image = QImage(tex_size, tex_size, QImage::Format_ARGB32);
    pixel_map_image.fill(0);
    PhGUtils::message("generating pixel map for albedo ...");
    boost::timer::auto_cpu_timer t("pixel map for albedo generation time = %w seconds.\n");

    for(int i=0;i<tex_size;++i) {
      for(int j=0;j<tex_size;++j) {
        double y = 1.0 - (i + 0.5) / static_cast<double>(tex_size);
        double x = (j + 0.5) / static_cast<double>(tex_size);

        QRgb pix = albedo_index_map.pixel(j, i);
        unsigned char r = static_cast<unsigned char>(qRed(pix));
        unsigned char g = static_cast<unsigned char>(qGreen(pix));
        unsigned char b = static_cast<unsigned char>(qBlue(pix));
        if(r == 0 && g == 0 && b == 0) continue;
        int fidx;
        decode_index(r, g, b, fidx);

        auto f = mesh.face_texture(fidx);
        auto t0 = mesh.texture_coords(f[0]), t1 = mesh.texture_coords(f[1]), t2 = mesh.texture_coords(f[2]);

        using PhGUtils::Point3f;
        using PhGUtils::Point2d;
        Point3f bcoords;
        // Compute barycentric coordinates
        PhGUtils::computeBarycentricCoordinates(Point2d(x, y),
                                                Point2d(t0[0], t0[1]), Point2d(t1[0], t1[1]), Point2d(t2[0], t2[1]),
                                                bcoords);
        //cerr << bcoords << endl;
        albedo_pixel_map[i][j] = PixelInfo(fidx, glm::vec3(bcoords.x, bcoords.y, bcoords.z));

        pixel_map_image.setPixel(j, i, qRgb(bcoords.x*255, bcoords.y*255, bcoords.z*255));
      }
      pixel_map_image.save("albedo_pixel.jpg");
    }
    PhGUtils::message("done.");
  }

  const string settings_filename(argv[1]);

  // Parse the setting file and load image related resources
  fs::path settings_filepath(settings_filename);

  vector<pair<string, string>> image_points_filenames = ParseSettingsFile(settings_filename);
  vector<ImageBundle> image_bundles;
  for(auto& p : image_points_filenames) {
    fs::path image_filename = settings_filepath.parent_path() / fs::path(p.first);
    fs::path pts_filename = settings_filepath.parent_path() / fs::path(p.second);
    fs::path res_filename = settings_filepath.parent_path() / fs::path(p.first + ".res");
    cout << "[" << image_filename << ", " << pts_filename << "]" << endl;

    auto image_points_pair = LoadImageAndPoints(image_filename.string(), pts_filename.string());
    auto recon_results = LoadReconstructionResult(res_filename.string());
    image_bundles.push_back(ImageBundle(image_points_pair.first, image_points_pair.second, recon_results));
  }

  MultilinearModel model(model_filename);

  // Collect texture information from each input (image, mesh) pair to obtain mean texture
  {
    // for each image bundle, render the mesh to FBO with culling to get the visible triangles

    // for each visible triangle, compute the coordinates of its 3 corners

    // for each pixel in the texture map, use backward projection to obtain pixel value in the input image

    // [Optional]: render the mesh with texture to verify the texel values

    // accumulate the texels in average texel map
  }

  // Shape from shading


  return 0;
}

#endif  // FACE_SHAPE_FROM_SHADING