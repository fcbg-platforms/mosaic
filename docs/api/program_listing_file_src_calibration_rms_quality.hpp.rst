
.. _program_listing_file_src_calibration_rms_quality.hpp:

Program Listing for File rms_quality.hpp
========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_calibration_rms_quality.hpp>` (``src\calibration\rms_quality.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   
   namespace mosaic {
   
   enum class RmsQuality { Excellent, Good, Acceptable, Poor };
   
   // Buckets a reprojection RMS error (px) using the thresholds already used by
   // CalibrationW's tooltip text: <0.5 excellent, <1.0 good, <2.0 acceptable,
   // else poor.
   [[nodiscard]] RmsQuality rms_quality_for(double rmsErrorPx);
   
   } // namespace mosaic
