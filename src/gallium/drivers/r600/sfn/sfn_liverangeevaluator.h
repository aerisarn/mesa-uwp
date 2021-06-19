#ifndef LIFERANGEEVALUATOR_H
#define LIFERANGEEVALUATOR_H

#include "sfn_valuefactory.h"

#include <map>
#include <cassert>

namespace r600 {

class Shader;

class LiveRangeEvaluator  {
public:

   LiveRangeEvaluator();

   LiveRangeMap run(Shader &sh);
};

}

#endif // LIFERANGEEVALUATOR_H
