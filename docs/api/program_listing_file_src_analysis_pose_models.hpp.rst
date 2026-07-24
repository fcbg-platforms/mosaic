
.. _program_listing_file_src_analysis_pose_models.hpp:

Program Listing for File pose_models.hpp
========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_analysis_pose_models.hpp>` (``src\analysis\pose_models.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QList>
   #include <QPair>
   #include <QString>
   
   namespace mosaic {
   
   /// @brief Display label / `--model` value pairs for the YOLOv8-pose variants
   /// shipped with `analysis/pose/human_pose.py`. Shared between every UI
   /// control that lets the user pick a pose model, so the option list only
   /// exists in one place.
   inline QList<QPair<QString, QString>> pose_model_options() {
       return {
           {"YOLOv8n-pose  (fastest, CPU OK)",  "yolov8n-pose.pt"},
           {"YOLOv8s-pose  (balanced)",         "yolov8s-pose.pt"},
           {"YOLOv8m-pose  (accurate, GPU)",    "yolov8m-pose.pt"},
           {"YOLOv8l-pose  (best, GPU req.)",   "yolov8l-pose.pt"},
       };
   }
   
   } // namespace mosaic
