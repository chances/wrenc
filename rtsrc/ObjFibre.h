//
// Created by znix on 28/07/22.
//

#pragma once

#include "Obj.h"

/**
 * A fibre is a thread of execution in Wren. Fibers can't execute concurrently, but they can be interwoven to
 * do stuff like making a stream of tokens (this example is from the Wren homepage):
 *
 * \code
 *   var adjectives = Fiber.new {
 *     ["small", "clean", "fast"].each {|word| Fiber.yield(word) }
 *   }
 *   while (!adjectives.isDone) System.print(adjectives.call())
 * \endcode
 *
 * There's effectively a 'stack of fibres': one fibre can call another fibre, which yields a value back to the
 * first fibre.
 *
 * Fibers here will have their own full callstack. Since many fibres are likely to be short-lived, we'll pool their
 * callstacks to avoid the number of times we have to allocate and deallocate callstacks.
 *
 * Note this is spelt with the American spelling ('Fiber') in the API, but with the British spelling in C++ (it's
 * slightly confusing, but I'm writing this and I get to use the dialect I like :P ).
 */
class ObjFibre : public Obj {
  public:
	~ObjFibre();
	ObjFibre();

	static ObjClass *Class();
};
