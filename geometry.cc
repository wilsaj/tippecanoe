#include <iostream>
#include <fstream>
#include <string>
#include <stack>
#include <vector>
#include <algorithm>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <sqlite3.h>
#include <limits.h>
#include "geometry.hh"
#include "clipper/clipper.hpp"

extern "C" {
#include "tile.h"
#include "clip.h"
#include "projection.h"
}

drawvec decode_geometry(char **meta, int z, unsigned tx, unsigned ty, int detail, long long *bbox) {
	drawvec out;

	bbox[0] = LONG_LONG_MAX;
	bbox[1] = LONG_LONG_MAX;
	bbox[2] = LONG_LONG_MIN;
	bbox[3] = LONG_LONG_MIN;

	long long wx = initial_x, wy = initial_y;

	while (1) {
		draw d;

		deserialize_byte(meta, &d.op);
		if (d.op == VT_END) {
			break;
		}

		if (d.op == VT_MOVETO || d.op == VT_LINETO) {
			long long dx, dy;

			deserialize_long_long(meta, &dx);
			deserialize_long_long(meta, &dy);

			wx += dx << geometry_scale;
			wy += dy << geometry_scale;

			long long wwx = wx;
			long long wwy = wy;

			if (z != 0) {
				wwx -= tx << (32 - z);
				wwy -= ty << (32 - z);
			}

			if (wwx < bbox[0]) {
				bbox[0] = wwx;
			}
			if (wwy < bbox[1]) {
				bbox[1] = wwy;
			}
			if (wwx > bbox[2]) {
				bbox[2] = wwx;
			}
			if (wwy > bbox[3]) {
				bbox[3] = wwy;
			}

			d.x = wwx;
			d.y = wwy;
		}

		out.push_back(d);
	}

	return out;
}

void to_tile_scale(drawvec &geom, int z, int detail) {
	unsigned i;

	for (i = 0; i < geom.size(); i++) {
		geom[i].x >>= (32 - detail - z);
		geom[i].y >>= (32 - detail - z);
	}
}

drawvec remove_noop(drawvec geom, int type, int shift) {
	// first pass: remove empty linetos

	long long x = 0, y = 0;
	drawvec out;
	unsigned i;

	for (i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_LINETO && (geom[i].x >> shift) == x && (geom[i].y >> shift) == y) {
			continue;
		}

		if (geom[i].op == VT_CLOSEPATH) {
			out.push_back(geom[i]);
		} else { /* moveto or lineto */
			out.push_back(geom[i]);
			x = geom[i].x >> shift;
			y = geom[i].y >> shift;
		}
	}

	// second pass: remove unused movetos

	geom = out;
	out.resize(0);

	for (i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			if (i + 1 >= geom.size()) {
				continue;
			}

			if (geom[i + 1].op == VT_MOVETO) {
				continue;
			}

			if (geom[i + 1].op == VT_CLOSEPATH) {
				fprintf(stderr, "Shouldn't happen\n");
				i++;  // also remove unused closepath
				continue;
			}
		}

		out.push_back(geom[i]);
	}

	// second pass: remove empty movetos

	if (type == VT_LINE) {
		geom = out;
		out.resize(0);

		for (i = 0; i < geom.size(); i++) {
			if (geom[i].op == VT_MOVETO) {
				if (i > 0 && geom[i - 1].op == VT_LINETO && (geom[i - 1].x >> shift) == (geom[i].x >> shift) && (geom[i - 1].y >> shift) == (geom[i].y >> shift)) {
					continue;
				}
			}

			out.push_back(geom[i]);
		}
	}

	return out;
}

/* XXX */
#if 0
drawvec shrink_lines(drawvec &geom, int z, int detail, int basezoom, long long *here, double droprate) {
	long long res = 200LL << (32 - 8 - z);
	long long portion = res / exp(log(sqrt(droprate)) * (basezoom - z));

	unsigned i;
	drawvec out;

	for (i = 0; i < geom.size(); i++) {
		if (i > 0 && (geom[i - 1].op == VT_MOVETO || geom[i - 1].op == VT_LINETO) && geom[i].op == VT_LINETO) {
			double dx = (geom[i].x - geom[i - 1].x);
			double dy = (geom[i].y - geom[i - 1].y);
			long long d = sqrt(dx * dx + dy * dy);

			long long n;
			long long next = LONG_LONG_MAX;
			for (n = *here; n < *here + d; n = next) {
				int within;

				if (n % res < portion) {
					next = (n / res) * res + portion;
					within = 1;
				} else {
					next = (n / res + 1) * res;
					within = 0;
				}

				if (next > *here + d) {
					next = *here + d;
				}

				//printf("drawing from %lld to %lld in %lld\n", n - *here, next - *here, d);

				double f1 = (n - *here) / (double) d;
				double f2 = (next - *here) / (double) d;

				if (within) {
					out.push_back(draw(VT_MOVETO, geom[i - 1].x + f1 * (geom[i].x - geom[i - 1].x), geom[i - 1].y + f1 * (geom[i].y - geom[i - 1].y)));
					out.push_back(draw(VT_LINETO, geom[i - 1].x + f2 * (geom[i].x - geom[i - 1].x), geom[i - 1].y + f2 * (geom[i].y - geom[i - 1].y)));
				} else {
					out.push_back(draw(VT_MOVETO, geom[i - 1].x + f2 * (geom[i].x - geom[i - 1].x), geom[i - 1].y + f2 * (geom[i].y - geom[i - 1].y)));
				}
			}

			*here += d;
		} else {
			out.push_back(geom[i]);
		}
	}

	return out;
}
#endif

static void decode_clipped(ClipperLib::PolyNode *t, drawvec &out) {
	// To make the GeoJSON come out right, we need to do each of the
	// outer rings followed by its children if any, and then go back
	// to do any outer-ring children of those children as a new top level.

	ClipperLib::Path p = t->Contour;
	for (unsigned i = 0; i < p.size(); i++) {
		out.push_back(draw((i == 0) ? VT_MOVETO : VT_LINETO, p[i].X, p[i].Y));
	}
	if (p.size() > 0) {
		out.push_back(draw(VT_LINETO, p[0].X, p[0].Y));
	}

	for (int n = 0; n < t->ChildCount(); n++) {
		ClipperLib::Path p = t->Childs[n]->Contour;
		for (unsigned i = 0; i < p.size(); i++) {
			out.push_back(draw((i == 0) ? VT_MOVETO : VT_LINETO, p[i].X, p[i].Y));
		}
		if (p.size() > 0) {
			out.push_back(draw(VT_LINETO, p[0].X, p[0].Y));
		}
	}

	for (int n = 0; n < t->ChildCount(); n++) {
		for (int m = 0; m < t->Childs[n]->ChildCount(); m++) {
			decode_clipped(t->Childs[n]->Childs[m], out);
		}
	}
}

drawvec clean_or_clip_poly(drawvec &geom, int z, int detail, int buffer, bool clip) {
	ClipperLib::Clipper clipper(ClipperLib::ioStrictlySimple);

	for (unsigned i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			unsigned j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			ClipperLib::Path path;

			drawvec tmp;
			for (unsigned k = i; k < j; k++) {
				path.push_back(ClipperLib::IntPoint(geom[k].x, geom[k].y));
			}

			if (!clipper.AddPath(path, ClipperLib::ptSubject, true)) {
#if 0
				fprintf(stderr, "Couldn't add polygon for clipping:");
				for (unsigned k = i; k < j; k++) {
					fprintf(stderr, " %lld,%lld", geom[k].x, geom[k].y);
				}
				fprintf(stderr, "\n");
#endif
			}

			i = j - 1;
		} else {
			fprintf(stderr, "Unexpected operation in polygon %d\n", (int) geom[i].op);
			exit(EXIT_FAILURE);
		}
	}

	if (clip) {
		long long area = 0xFFFFFFFF;
		if (z != 0) {
			area = 1LL << (32 - z);
		}
		long long clip_buffer = buffer * area / 256;

		ClipperLib::Path edge;
		edge.push_back(ClipperLib::IntPoint(-clip_buffer, -clip_buffer));
		edge.push_back(ClipperLib::IntPoint(area + clip_buffer, -clip_buffer));
		edge.push_back(ClipperLib::IntPoint(area + clip_buffer, area + clip_buffer));
		edge.push_back(ClipperLib::IntPoint(-clip_buffer, area + clip_buffer));
		edge.push_back(ClipperLib::IntPoint(-clip_buffer, -clip_buffer));

		clipper.AddPath(edge, ClipperLib::ptClip, true);
	}

	ClipperLib::PolyTree clipped;
	if (clip) {
		if (!clipper.Execute(ClipperLib::ctIntersection, clipped)) {
			fprintf(stderr, "Polygon clip failed\n");
		}
	} else {
		if (!clipper.Execute(ClipperLib::ctUnion, clipped)) {
			fprintf(stderr, "Polygon clean failed\n");
		}
	}

	drawvec out;

	for (int i = 0; i < clipped.ChildCount(); i++) {
		decode_clipped(clipped.Childs[i], out);
	}

	return out;
}

drawvec close_poly(drawvec &geom) {
	drawvec out;

	for (unsigned i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			unsigned j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			if (j - 1 > i) {
				if (geom[j - 1].x != geom[i].x || geom[j - 1].y != geom[i].y) {
					fprintf(stderr, "Internal error: polygon not closed\n");
				}
			}

			for (unsigned n = i; n < j - 1; n++) {
				out.push_back(geom[n]);
			}
			out.push_back(draw(VT_CLOSEPATH, 0, 0));

			i = j - 1;
		}
	}

	return out;
}

drawvec reduce_tiny_poly(drawvec &geom, int z, int detail, bool *reduced, double *accum_area) {
	drawvec out;
	long long pixel = (1 << (32 - detail - z)) * 2;

	*reduced = true;
	bool included_last_outer = false;

	for (unsigned i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			unsigned j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			double area = 0;
			for (unsigned k = i; k < j; k++) {
				area += geom[k].x * geom[i + ((k - i + 1) % (j - i))].y;
				area -= geom[k].y * geom[i + ((k - i + 1) % (j - i))].x;
			}
			area = area / 2;

			if (fabs(area) <= pixel * pixel || (area < 0 && !included_last_outer)) {
				// printf("area is only %f vs %lld so using square\n", area, pixel * pixel);

				*accum_area += area;
				if (*accum_area > pixel * pixel) {
					// XXX use centroid;

					out.push_back(draw(VT_MOVETO, geom[i].x - pixel / 2, geom[i].y - pixel / 2));
					out.push_back(draw(VT_LINETO, geom[i].x + pixel / 2, geom[i].y - pixel / 2));
					out.push_back(draw(VT_LINETO, geom[i].x + pixel / 2, geom[i].y + pixel / 2));
					out.push_back(draw(VT_LINETO, geom[i].x - pixel / 2, geom[i].y + pixel / 2));
					out.push_back(draw(VT_LINETO, geom[i].x - pixel / 2, geom[i].y - pixel / 2));

					*accum_area -= pixel * pixel;
				}

				if (area >= 0) {
					included_last_outer = false;
				}
			} else {
				// printf("area is %f so keeping instead of %lld\n", area, pixel * pixel);

				for (unsigned k = i; k <= j && k < geom.size(); k++) {
					out.push_back(geom[k]);
				}

				*reduced = false;

				if (area >= 0) {
					included_last_outer = true;
				}
			}

			i = j - 1;
		} else {
			fprintf(stderr, "how did we get here with %d in %d?\n", geom[i].op, (int) geom.size());

			for (unsigned n = 0; n < geom.size(); n++) {
				fprintf(stderr, "%d/%lld/%lld ", geom[n].op, geom[n].x, geom[n].y);
			}
			fprintf(stderr, "\n");

			out.push_back(geom[i]);
		}
	}

	return out;
}

drawvec clip_point(drawvec &geom, int z, int detail, long long buffer) {
	drawvec out;
	unsigned i;

	long long min = 0;
	long long area = 0xFFFFFFFF;
	if (z != 0) {
		area = 1LL << (32 - z);

		min -= buffer * area / 256;
		area += buffer * area / 256;
	}

	for (i = 0; i < geom.size(); i++) {
		if (geom[i].x >= min && geom[i].y >= min && geom[i].x <= area && geom[i].y <= area) {
			out.push_back(geom[i]);
		}
	}

	return out;
}

int quick_check(long long *bbox, int z, int detail, long long buffer) {
	long long min = 0;
	long long area = 0xFFFFFFFF;
	if (z != 0) {
		area = 1LL << (32 - z);

		min -= buffer * area / 256;
		area += buffer * area / 256;
	}

	// bbox entirely outside the tile
	if (bbox[0] > area || bbox[1] > area) {
		return 0;
	}
	if (bbox[2] < min || bbox[3] < min) {
		return 0;
	}

	// bbox entirely within the tile
	if (bbox[0] > min && bbox[1] > min && bbox[2] < area && bbox[3] < area) {
		return 1;
	}

	// some overlap of edge
	return 2;
}

drawvec clip_lines(drawvec &geom, int z, int detail, long long buffer) {
	drawvec out;
	unsigned i;

	long long min = 0;
	long long area = 0xFFFFFFFF;
	if (z != 0) {
		area = 1LL << (32 - z);

		min -= buffer * area / 256;
		area += buffer * area / 256;
	}

	for (i = 0; i < geom.size(); i++) {
		if (i > 0 && (geom[i - 1].op == VT_MOVETO || geom[i - 1].op == VT_LINETO) && geom[i].op == VT_LINETO) {
			double x1 = geom[i - 1].x;
			double y1 = geom[i - 1].y;

			double x2 = geom[i - 0].x;
			double y2 = geom[i - 0].y;

			int c = clip(&x1, &y1, &x2, &y2, min, min, area, area);

			if (c > 1) {  // clipped
				out.push_back(draw(VT_MOVETO, x1, y1));
				out.push_back(draw(VT_LINETO, x2, y2));
				out.push_back(draw(VT_MOVETO, geom[i].x, geom[i].y));
			} else if (c == 1) {  // unchanged
				out.push_back(geom[i]);
			} else {  // clipped away entirely
				out.push_back(draw(VT_MOVETO, geom[i].x, geom[i].y));
			}
		} else {
			out.push_back(geom[i]);
		}
	}

	return out;
}

static double square_distance_from_line(long long point_x, long long point_y, long long segA_x, long long segA_y, long long segB_x, long long segB_y) {
	double p2x = segB_x - segA_x;
	double p2y = segB_y - segA_y;
	double something = p2x * p2x + p2y * p2y;
	double u = 0 == something ? 0 : ((point_x - segA_x) * p2x + (point_y - segA_y) * p2y) / something;

	if (u > 1) {
		u = 1;
	} else if (u < 0) {
		u = 0;
	}

	double x = segA_x + u * p2x;
	double y = segA_y + u * p2y;

	double dx = x - point_x;
	double dy = y - point_y;

	return dx * dx + dy * dy;
}

// https://github.com/Project-OSRM/osrm-backend/blob/733d1384a40f/Algorithms/DouglasePeucker.cpp
static void douglas_peucker(drawvec &geom, int start, int n, double e) {
	e = e * e;
	std::stack<int> recursion_stack;

	{
		int left_border = 0;
		int right_border = 1;
		// Sweep linerarily over array and identify those ranges that need to be checked
		do {
			if (geom[start + right_border].necessary) {
				recursion_stack.push(left_border);
				recursion_stack.push(right_border);
				left_border = right_border;
			}
			++right_border;
		} while (right_border < n);
	}

	while (!recursion_stack.empty()) {
		// pop next element
		int second = recursion_stack.top();
		recursion_stack.pop();
		int first = recursion_stack.top();
		recursion_stack.pop();

		double max_distance = -1;
		int farthest_element_index = second;

		// find index idx of element with max_distance
		int i;
		for (i = first + 1; i < second; i++) {
			double temp_dist = square_distance_from_line(geom[start + i].x, geom[start + i].y, geom[start + first].x, geom[start + first].y, geom[start + second].x, geom[start + second].y);

			double distance = fabs(temp_dist);

			if (distance > e && distance > max_distance) {
				farthest_element_index = i;
				max_distance = distance;
			}
		}

		if (max_distance > e) {
			// mark idx as necessary
			geom[start + farthest_element_index].necessary = 1;

			if (1 < farthest_element_index - first) {
				recursion_stack.push(first);
				recursion_stack.push(farthest_element_index);
			}
			if (1 < second - farthest_element_index) {
				recursion_stack.push(farthest_element_index);
				recursion_stack.push(second);
			}
		}
	}
}

drawvec simplify_lines(drawvec &geom, int z, int detail) {
	int res = 1 << (32 - detail - z);

	unsigned i;
	for (i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			geom[i].necessary = 1;
		} else if (geom[i].op == VT_LINETO) {
			geom[i].necessary = 0;
		} else {
			geom[i].necessary = 1;
		}
	}

	for (i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			unsigned j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			geom[i].necessary = 1;
			geom[j - 1].necessary = 1;

			douglas_peucker(geom, i, j - i, res);
			i = j - 1;
		}
	}

	drawvec out;
	for (i = 0; i < geom.size(); i++) {
		if (geom[i].necessary) {
			out.push_back(geom[i]);
		}
	}

	return out;
}

drawvec reorder_lines(drawvec &geom) {
	// Only reorder simple linestrings with a single moveto

	if (geom.size() == 0) {
		return geom;
	}

	unsigned i;
	for (i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			if (i != 0) {
				return geom;
			}
		} else if (geom[i].op == VT_LINETO) {
			if (i == 0) {
				return geom;
			}
		} else {
			return geom;
		}
	}

	// Reorder anything that goes up and to the left
	// instead of down and to the right
	// so that it will coalesce better

	unsigned long long l1 = encode(geom[0].x, geom[0].y);
	unsigned long long l2 = encode(geom[geom.size() - 1].x, geom[geom.size() - 1].y);

	if (l1 > l2) {
		drawvec out;
		for (i = 0; i < geom.size(); i++) {
			out.push_back(geom[geom.size() - 1 - i]);
		}
		out[0].op = VT_MOVETO;
		out[out.size() - 1].op = VT_LINETO;
		return out;
	}

	return geom;
}

drawvec fix_polygon(drawvec &geom) {
	int outer = 1;
	drawvec out;

	unsigned i;
	for (i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_CLOSEPATH) {
			outer = 1;
		} else if (geom[i].op == VT_MOVETO) {
			// Find the end of the ring

			unsigned j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			// Make a temporary copy of the ring.
			// Close it if it isn't closed.

			drawvec ring;
			for (unsigned a = i; a < j; a++) {
				ring.push_back(geom[a]);
			}
			if (j - i != 0 && (ring[0].x != ring[j - i - 1].x || ring[0].y != ring[j - i - 1].y)) {
				ring.push_back(ring[0]);
			}

			// Reverse ring if winding order doesn't match
			// inner/outer expectation

			double area = 0;
			for (unsigned k = 0; k < ring.size(); k++) {
				area += (long double) ring[k].x * (long double) ring[(k + 1) % ring.size()].y;
				area -= (long double) ring[k].y * (long double) ring[(k + 1) % ring.size()].x;
			}

			if ((area > 0) != outer) {
				drawvec tmp;
				for (int a = ring.size() - 1; a >= 0; a--) {
					tmp.push_back(ring[a]);
				}
				ring = tmp;
			}

			// Copy ring into output, fixing the moveto/lineto ops if necessary because of
			// reversal or closing

			for (unsigned a = 0; a < ring.size(); a++) {
				if (a == 0) {
					out.push_back(draw(VT_MOVETO, ring[a].x, ring[a].y));
				} else {
					out.push_back(draw(VT_LINETO, ring[a].x, ring[a].y));
				}
			}

			// Next ring or polygon begins on the non-lineto that ended this one
			// and is not an outer ring unless there is a terminator first

			i = j - 1;
			outer = 0;
		} else {
			fprintf(stderr, "Internal error: polygon ring begins with %d, not moveto\n", geom[i].op);
			exit(EXIT_FAILURE);
		}
	}

	return out;
}
