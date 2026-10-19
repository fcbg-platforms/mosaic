#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

namespace mosaic {

/// @brief What analysis/run_social.py found about two people
/// (``<session>/social/social.json``, schema mosaic-social-v1): synchrony of
/// head movement, body movement and heart rate, and smile and nod mimicry.
/// See docs/math/social_interaction.rst.
class SocialResult {
   public:
    /// One windowed cross-correlation measure (head, body or heart rate).
    struct Measure {
        QString id;            ///< "head", "body" or "heart_rate"
        double t0Ms{};         ///< the grid's start, session elapsed ms
        QVector<double> tS;    ///< window centres, seconds from t0Ms
        QVector<double> peakR; ///< peak correlation per window (NaN: too little data)
        QVector<double> lagS;  ///< its lag; positive: B follows A
        double meanPeakR{};
        std::optional<double> surrogateMeanPeakR;
        std::optional<double> surrogateP95;
        std::optional<double> synchrony; ///< real minus surrogates' mean
        std::optional<double> pValue;
        std::optional<double> aLeadsPct;
        std::optional<double> bLeadsPct;
        std::optional<double> medianLagS;
        int windows{};
    };

    /// One mimicry direction for one kind of event.
    struct Mimicry {
        QString kind;     ///< "smile" or "nod"
        bool aToB = true; ///< B following A (else A following B)
        int events{};     ///< the leader's events counted
        int followed{};
        std::optional<double> rate;
        std::optional<double> chanceRate;
        std::optional<double> pValue;
    };

    [[nodiscard]] static SocialResult load(const QString& path);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] int camera_a() const { return cameraA_; }
    [[nodiscard]] int camera_b() const { return cameraB_; }
    /// The first frame of each camera's video, session elapsed ms (to put
    /// window times on that video's own clock); NaN when unknown.
    [[nodiscard]] double video_start_ms(int camera) const;
    [[nodiscard]] const QVector<Measure>& measures() const { return measures_; }
    [[nodiscard]] const Measure* measure(const QString& id) const;
    [[nodiscard]] const QVector<Mimicry>& mimicry() const { return mimicry_; }
    [[nodiscard]] const QStringList& notes() const { return notes_; }

   private:
    bool valid_  = false;
    int cameraA_ = -1;
    int cameraB_ = -1;
    double startA_{};
    double startB_{};
    QVector<Measure> measures_;
    QVector<Mimicry> mimicry_;
    QStringList notes_;
};

} // namespace mosaic
