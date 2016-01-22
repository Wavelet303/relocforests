#pragma once

#include "node.hpp"
#include "random.hpp"
#include "settings.hpp"
#include "data.hpp"
#include "MeanShift.hpp"

#include <vector>
#include <cstdint>
#include <ctime>

#include <unordered_map>

namespace ISUE {
  namespace RelocForests {
    class Point3D {
    public:
      Point3D(double x, double y, double z) : x(x), y(y), z(z) {};
      double x, y, z;
    };

    struct hashFunc{
      size_t operator()(const Point3D &k) const {
        size_t h1 = std::hash<double>()(k.x);
        size_t h2 = std::hash<double>()(k.y);
        size_t h3 = std::hash<double>()(k.z);
        return (h1 ^ (h2 << 1)) ^ h3;
      }
    };

    struct equalsFunc {
      bool operator()(const Point3D &l, const Point3D &r) const{
        return (l.x == r.x) && (l.y == r.y) && (l.z == r.z);
      }
    };

    typedef std::unordered_map<Point3D, uint32_t, hashFunc, equalsFunc> Point3DMap;


    class Tree {
    public:
      Tree()
      {
        root_ = new Node();
      };

      ~Tree()
      {
        delete root_;
      };

      void WriteTree(std::ostream& o, Node *node) const
      {
        if (node == nullptr)
          o.write((const char*)"null", sizeof("null"));
        node->Serialize(o);
        WriteTree(o, node->left_);
        WriteTree(o, node->right_);
      }

      void Serialize(std::ostream& stream) const
      {
        const int majorVersion = 0, minorVersion = 0;
        
        stream.write(binaryFileHeader_, strlen(binaryFileHeader_));
        stream.write((const char*)(&majorVersion), sizeof(majorVersion));
        stream.write((const char*)(&minorVersion), sizeof(minorVersion));

        stream.write((const char*)(&settings_->max_tree_depth_), sizeof(settings_->max_tree_depth_));

        WriteTree(stream, root_);
      }


      // learner output
      enum DECISION { LEFT, RIGHT, TRASH };

      //  Evaluates weak learner. Decides whether the point should go left or right.
      //  Returns DECISION enum value.
      //DECISION eval_learner(Data *data, LabeledPixel pixel, DepthAdaptiveRGB *feature) 
      DECISION eval_learner(DepthAdaptiveRGB feature, cv::Mat depth_image, cv::Mat rgb_image, cv::Point2i pos)
      {
        bool valid = true;
        float response = feature.GetResponse(depth_image, rgb_image, pos, valid);

        if (!valid) // no depth or out of bounds
          return DECISION::TRASH;

        return (DECISION)(response >= feature.GetThreshold());
      }

      // V(S)
      double variance(std::vector<LabeledPixel> labeled_data)
      {
        if (labeled_data.size() == 0)
          return 0.0;
        double V = (1.0f / (double)labeled_data.size());
        double sum = 0.0f;

        // calculate mean of S
        cv::Point3f tmp;
        for (auto p : labeled_data)
          tmp += p.label_;
        uint32_t size = labeled_data.size();
        cv::Point3f mean(tmp.x / size, tmp.y / size, tmp.z / size);

        for (auto p : labeled_data) {
          cv::Point3f val = (p.label_ - mean);
          sum += val.x * val.x  + val.y * val.y + val.z * val.z;
        }

        return V * sum;
      }


      // Q(S_n, \theta)
      double objective_function(std::vector<LabeledPixel> data, std::vector<LabeledPixel> left, std::vector<LabeledPixel> right)
      {
        double var = variance(data);
        double left_val = ((double)left.size() / (double)data.size()) * variance(left);
        double right_val = ((double)right.size() / (double)data.size()) * variance(right);

        return var - (left_val + right_val);
      }

      // Returns height from current node to root node.
      uint32_t traverse_to_root(Node *node) {
        if (node == nullptr)
          return 0;
        return 1 + traverse_to_root(node->parent_);
      }


      void train_recurse(Node *node, std::vector<LabeledPixel> S) 
      {
        uint16_t height = traverse_to_root(node);
        if (S.size() == 1 || height >= settings_->max_tree_depth_) {

          std::vector<Eigen::Vector3d> data;

          // calc mode for leaf, sub-sample N_SS = 500
          for (uint16_t i = 0; i < (S.size() < 500 ? S.size() : 500); i++) {
            auto p = S.at(i);
            Eigen::Vector3d point { p.label_.x, p.label_.y, p.label_.z };
            data.push_back(point);
          }

          // cluster
          MeanShift *ms = new MeanShift(nullptr);
          double kernel_bandwidth = 0.01f; // gaussian
          std::vector<Eigen::Vector3d> cluster = ms->cluster(data, kernel_bandwidth);

          // find mode
          std::vector<Point3D> clustered_points;
          for (auto c : cluster)
            clustered_points.push_back(Point3D(floor(c[0] * 10000) / 10000, floor(c[1] * 10000) / 10000, floor(c[2] * 10000) / 10000));

          Point3DMap cluster_map;

          for (auto p : clustered_points)
            cluster_map[p]++;

          std::pair<Point3D, uint32_t> mode(Point3D(0.0, 0.0, 0.0), 0);

          for (auto p : cluster_map)
            if (p.second > mode.second)
              mode = p;


          node->mode_ = Eigen::Vector3d(mode.first.x, mode.first.y, mode.first.z);
          node->is_leaf_ = true;
          return;
        }
        else if (S.size() == 0) {
          delete node;
          node = nullptr;
          return;
        }

        node->is_split_ = true;
        node->is_leaf_ = false;

        uint32_t num_candidates = 5,
                feature = 0;
        double minimum_objective = DBL_MAX;

        std::vector<DepthAdaptiveRGB> candidate_params;
        std::vector<LabeledPixel> left_final, right_final;


        for (uint32_t i = 0; i < num_candidates; ++i) {

          // add candidate
          candidate_params.push_back(DepthAdaptiveRGB::CreateRandom(random_));

          // partition data with candidate
          std::vector<LabeledPixel> left_data, right_data;

          for (uint32_t j = 0; j < S.size(); ++j) {

            LabeledPixel p = S.at(j);
            DECISION val = eval_learner(candidate_params.at(i), data_->GetDepthImage(p.frame_), data_->GetRGBImage(p.frame_), p.pos_);

            switch (val) {
            case LEFT:
              left_data.push_back(S.at(j));
              break;
            case RIGHT:
              right_data.push_back(S.at(j));
              break;
            case TRASH:
              // do nothing
              break;
            }

          }

          // eval tree training objective function and take best
          // todo: ensure objective function is correct
          double objective = objective_function(S, left_data, right_data);

          if (objective < minimum_objective) {
            feature = i;
            minimum_objective= objective;
            left_final = left_data;
            right_final = right_data;
          }
        }

        // set feature
        node->feature_ = candidate_params.at(feature);
        node->left_ = new Node();
        node->right_ = new Node();
        node->left_->parent_ = node->right_->parent_ = node;

        train_recurse(node->left_, left_final);
        train_recurse(node->right_, right_final);
      }

      void Train(Data *data, std::vector<LabeledPixel> labeled_data, Random *random, Settings *settings) 
      {
        data_ = data;
        random_ = random;
        settings_ = settings;
        train_recurse(root_, labeled_data);
      }

      Eigen::Vector3d eval_recursive(Node *node, int row, int col, cv::Mat rgb_image, cv::Mat depth_image)
      {

        if (node->is_leaf_) {
          return node->mode_;
        }

        DECISION val = eval_learner(node->feature_, depth_image, rgb_image, cv::Point2i(col, row));

        switch (val) {
        case LEFT:
          eval_recursive(node->left_, row, col, rgb_image, depth_image);
          break;
        case RIGHT:
          eval_recursive(node->right_, row, col, rgb_image, depth_image);
          break;
        case TRASH:
          break;
        }
      }

      // Evaluate tree at a pixel
      Eigen::Vector3d Eval(int row, int col, cv::Mat rgb_image, cv::Mat depth_image)
      {
        return eval_recursive(root_, row, col, rgb_image, depth_image);
      }


    private:
      Node *root_;
      Data *data_;
      Random *random_;
      Settings *settings_;
      const char* binaryFileHeader_ = "ISUE.RelocForests.Tree";
    };
  }
}
