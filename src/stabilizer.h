#pragma once
#include <deque>
#include <vector>
#include "brush.h"

// Krita's Stabilizer without the dead zone (#246, /r/14): the mean of the last n
// pen samples. sensors on averages pressure and tilt too; off, they are the newest
// sample's. n = 0 passes samples through.
struct Stabilizer
{
  int n = 0;
  bool sensors = false;
  std::deque<PenSample> q;

  void push(const PenSample &s)
  {
    q.push_back(s);
    while (int(q.size()) > (n > 0 ? n : 1))
      q.pop_front();
  }

  // The plain mean of the queue; dt is the newest sample's. Call only after a push.
  PenSample sample() const
  {
    PenSample m = q.back();
    float x = 0, y = 0, p = 0, tx = 0, ty = 0;
    for (auto &s : q)
      x += s.x, y += s.y, p += s.pressure, tx += s.xtilt, ty += s.ytilt;
    float k = float(q.size());
    m.x = x / k, m.y = y / k;
    if (sensors)
      m.pressure = p / k, m.xtilt = tx / k, m.ytilt = ty / k;
    return m;
  }

  // Pen-up: the samples that flush the queue with the last pushed one, so the line
  // ends exactly there. Empties the queue.
  std::vector<PenSample> finish()
  {
    std::vector<PenSample> out;
    if (q.empty())
      return out;
    PenSample last = q.back();
    for (int i = 0; i < n; ++i)
      push(last), out.push_back(sample());
    // n copies of last fill the queue; pin it so float rounding in the mean cannot
    // leave it short.
    if (!out.empty())
      out.back() = last;
    q.clear();
    return out;
  }
};
