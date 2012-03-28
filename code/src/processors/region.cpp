/**
 * region.cpp
 *
 * Toke Høiland-Jørgensen
 * 2012-03-25
 */

#include "region.h"

using namespace ImageProcessing;

Region::Region()
{
}

Region::Region(const Mat &m, bool mask)
{
  qDebug("Region(Mat,bool) constructor");

  Size s; Point p;
  RPoint min, max;
  // Get the position in the parent matrix if one exists, and use that
  // as an offset for computing the actual points. Allows to use a
  // sub-region matrix to create a matrix from.
  m.locateROI(s,p);

  // We don't want reallocations as we're adding stuff, so reserve
  // space for a rectangle going all the way around the region, plus
  // some extra for odd paths.
  points.reserve(qRound((s.width+s.height)*2.5));
  if(mask) {
    for(int i = 0; i < s.height; i++) {
      const uchar *ptr = m.ptr<uchar>(i);
      for(int j = 0; j < s.width; j++) {
        if(ptr[j] && // if the point is set
           // and it is an edge point
           (i == 0 || // edges of regions
            j == 0 ||
            i == s.height-1 ||
            j == s.width-1 ||
            (!ptr[j-1]) || (!ptr[j+1]) || // before or after x-value not set
            (!m.at<int>(j,i-1)) || (!m.at<int>(j,i+1)) // before or after y-value not set
            )
           ) {
          points.append(RPoint(j+p.x,i+p.y));
        }
      }
    }
    bound_min = points.first();
    bound_max = points.last();
  } else {
    bound_min = RPoint(p.x, p.y);
    bound_max = RPoint(p.x+s.width, p.y+s.height);

    // do first column, then middle ones, then last column, so
    // preserve sorted order.
    int i;
    for(i = 0; i < s.width; i++) {
      points.append(RPoint(p.x+i,p.y));
    }
    for(i = 1; i < s.height-1; i++) {
      points.append(RPoint(p.x, p.y+i));
      points.append(RPoint(p.x+s.width-1, p.y+i));
    }
    for(i = 0; i < s.width; i++) {
      points.append(RPoint(p.x+i,p.y+s.height-1));
    }
  }

  buildYMap();
}

Region::Region(const Region &r)
{
  bound_min = r.bound_min;
  bound_max = r.bound_max;
  points = r.points; // Qt implicit sharing makes this safe.
  ycoords = r.ycoords;
}

Region::~Region()
{
}

void Region::add(const Region &other)
{
  if(!adjacentTo(other)) return;
  // TODO: Add this
}

void Region::add(RPoint p)
{
  if(contains(p)) return;
  // TODO: Add this
}

bool Region::isEmpty() const
{
  return points.size() == 0;
}

Mat Region::toMask() const
{
  // Create a new matrix large enough to hold the rectangle up to the
  // max of the bounds.
  Mat m = Mat::zeros(bound_max.y()-1, bound_max.x()-1, CV_8UC1);

  // Build up the array line by line, by going through all possible
  // points in the bounding rectangle. Keep an array that for each
  // x-value keeps track of whether or not, on the last row, this
  // x-value was in the bounding set, and whether or not this x-value
  // is currently inside the region.

  char *xmap = new char[bound_max.x()-bound_min.x()];
  for(int i = bound_min.y(); i <= bound_max.y(); i++) {
    for(int j = bound_min.x(); j <= bound_max.x(); j++) {
      RPoint p(j,i);
      if(inBoundary(p)) {
        xmap[j] |= 1;
      } else {
        if(xmap[j] & 1 && contains(p)) {
          xmap[j] |= 2;
        }
        xmap[j] &= (~1);
      }
      if(xmap[j] & 3) m.at<uchar>(j, i) = 255;
    }
  }

  return m;
}

/**
 * Check whether another region is adjacent to this one.
 *
 * Region b is adjacent to region a if, for at least one point p in
 * the boundary of a, the boundary of b contains a point that is a
 * 4-neighbour of p. Since regions contain only boundary points, it is
 * straight-forward to check for this.
 *
 * As an optimisation, if the bounding regions are entirely disjoint,
 * to not check all the points.
 **/
bool Region::adjacentTo(const Region &other) const
{
  // Step 1. Check if regions are entirely disjoint.
  //
  // Note that a < comparison on the points themselves are not enough,
  // because two points can have equal x coordinates and still be <
  // each other.
  if((other.bound_max.x() < bound_min.x() && other.bound_max.y() < bound_min.y()) ||
     (bound_max.x() < other.bound_min.x() && bound_max.y() < other.bound_min.y()))
    return false;

  for(int i = 0; i < points.size(); i++) {
    if(adjacentPoint(points[i], other)) return true;
  }
  return false;
}

bool Region::adjacentPoint(const RPoint p) const
{
  return (inBoundary(p+RPoint(-1, 0)) ||
          inBoundary(p+RPoint(+1, 0)) ||
          inBoundary(p+RPoint(0, -1)) ||
          inBoundary(p+RPoint(0, 1)));
}

/**
 * Checks whether a given point is contained in the region.
 *
 * A point is in the region if it is in the boundary or the interior.
 * As an optimisation, check if the point is entirely outside the
 * bounding box first.
 *
 */
bool Region::contains(const RPoint p) const
{
  if(p < bound_min || bound_max < p) return false;
  return inBoundary(p) || interior(p);
}

/**
 * Checks whether a given point is part of the bounding set.
 *
 * This uses the sorted nature of the points to do a smarter lookup
 * than a naive points.contains().
 */
bool Region::inBoundary(const RPoint p) const
{
  // If no points with this y coordinate are in the region, this point
  // is not.
  if(!ycoords.contains(p.y())) return false;
  for(int i = ycoords.value(p.y()); i < points.size() && i == p.y(); i++) {
    if(points[i] == p) return true;
  }
  return false;
}

/**
 * Check if a point is in the interior of the region.
 *
 * Try extending a line from the points in each x and y direction.
 * These lines each has to hit a point in the boundary set. If they do
 * not (i.e. the lines cross the bounding rectangle before a match is
 * found), the point is not in the interior.
 */
bool Region::interior(const RPoint p) const
{
  // keep track of each direction
  bool x_plus = false, x_minus = false, y_plus = false, y_minus = false;

  // Loop until we've found a match in each direction
  for(int i = 1; !x_plus && !x_minus && !y_plus && !y_minus; i++) {
    if(!x_plus && inBoundary(p+RPoint(i,0))) x_plus = true;
    if(!x_minus && inBoundary(p+RPoint(-i,0))) x_minus = true;
    if(!y_plus && inBoundary(p+RPoint(0,i))) y_plus = true;
    if(!y_minus && inBoundary(p+RPoint(0,-i))) y_minus = true;

    // If we moved outside the bounding box, the point is not in the
    // interior.
    if(p.x()-i < bound_min.x() && p.x()+i > bound_max.x() &&
       p.y()-i < bound_min.y() && p.y()+i > bound_max.y()) return false;
  }
  return true;
}

/**
 * Build up the map of Y coordinates to points list positions.
 * The map is used for efficient lookup of points.
 */
void Region::buildYMap()
{
  ycoords.clear();
  if(points.size() == 0) return;
  int i, current;
  current = points[0].y();
  ycoords.insert(current, 0);
  for(i = 0; i < points.size(); i++) {
    if(current != points[i].y()) {
      current = points[i].y();
      ycoords.insert(current, i);
    }
  }
}
