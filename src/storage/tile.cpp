/*-------------------------------------------------------------------------
 *
 * tile.cpp
 * the base class for all tiles
 *
 * Copyright(c) 2015, CMU
 *
 * /n-store/src/storage/tile.cpp
 *
 *-------------------------------------------------------------------------
 */

#include <sstream>
#include <cassert>
#include <cstdio>


#include "common/exception.h"
#include "common/pool.h"
#include "common/serializer.h"
#include "catalog/schema.h"
#include "storage/tuple.h"
#include "storage/tile.h"
#include "storage/tile_iterator.h"

namespace nstore {
namespace storage {

Tile::Tile(TileGroupHeader* _tile_header, Backend* _backend, catalog::Schema *tuple_schema,
		int tuple_count, const std::vector<std::string>& _columns_names, bool _own_schema)
: backend(_backend),
  data(NULL),
  pool(NULL),
  column_names(_columns_names),
  schema(tuple_schema),
  own_schema(_own_schema),
  num_tuple_slots(0),
  column_count(tuple_schema->GetColumnCount()),
  tuple_length(tuple_schema->GetLength()),
  uninlined_data_size(0),
  tile_id(INVALID_ID),
  tile_group_id(INVALID_ID),
  table_id(INVALID_ID),
  database_id(INVALID_ID),
  column_header(NULL),
  column_header_size(INVALID_ID),
  tile_group_header(_tile_header){
	assert(tuple_count > 0);

	tile_size = tuple_count * tuple_length;

	// allocate tuple storage space for inlined data
	data = (char *) backend->Allocate(tile_size);
	assert(data != NULL);

	// initialize it
	std::memset(data, 0, tile_size);
	num_tuple_slots = tuple_count;

	// allocate default pool if schema not inlined
	if(schema->IsInlined() == false)
		pool = new Pool(backend);
}

Tile::~Tile() {
	// reclaim the tile memory (only inlined data)
	backend->Free(data);
	data = NULL;

	// release pool storage if schema not inlined
	if(schema->IsInlined() == false)
		delete pool;
	pool = NULL;

	// release schema if owned. Do this at the end.
	if(own_schema)
		delete schema;

	// clear any cached column headers
	if (column_header)
		delete column_header;
	column_header = NULL;

}


//===--------------------------------------------------------------------===//
// Tuples
//===--------------------------------------------------------------------===//

/**
 * Insert tuple at slot
 * NOTE : No checks, must be at valid slot.
 */
void Tile::InsertTuple(const id_t tuple_slot_id, Tuple *tuple) {
	// Find slot location
	char *location = tuple_slot_id * tuple_length + data;

	std::memcpy(location, tuple->tuple_data, tuple_length);
}

/**
 * Returns tuple present at slot
 * NOTE : No checks, must be at valid slot and must exist.
 */
Tuple *Tile::GetTuple(const id_t tuple_slot_id) {

	storage::Tuple *tuple = new storage::Tuple(schema, true);

	tuple->Copy(GetTupleLocation(tuple_slot_id), pool);

	return tuple;
}


int Tile::GetColumnOffset(const std::string &name) const {
	for (id_t column_itr = 0, cnt = column_count; column_itr < cnt; column_itr++) {
		if (column_names[column_itr].compare(name) == 0) {
			return column_itr;
		}
	}

	return -1;
}

//===--------------------------------------------------------------------===//
// Utilities
//===--------------------------------------------------------------------===//

// Get a string representation of this tile
std::ostream& operator<<(std::ostream& os, const Tile& tile) {

	os << "\t-----------------------------------------------------------\n";

	os << "\tTILE\n";
	os << "\tCatalog ::"
			<< " Backend: " << tile.backend->GetBackendType()
			<< " DB: "<< tile.database_id << " Table: " << tile.table_id
			<< " Tile Group:  " << tile.tile_group_id
			<< " Tile:  " << tile.tile_id
			<< "\n";

	// Is it a dynamic tile or static tile ?
	if(tile.tile_group_header != nullptr) {
		os << "\tActive Tuples:  " << tile.tile_group_header->GetActiveTupleCount()
					<< " out of " << tile.num_tuple_slots  <<" slots\n";
	}
	else {
		os << "\tActive Tuples:  " << tile.num_tuple_slots  <<" slots\n";
	}

	// Columns
	// os << "\t-----------------------------------------------------------\n";
	// os << "\tSCHEMA\n";
	// os << (*tile.schema);

	// Tuples
	os << "\t-----------------------------------------------------------\n";
	os << "\tDATA\n";

	TileIterator tile_itr(&tile);
	Tuple tuple(tile.schema);

	std::string last_tuple = "";

	while (tile_itr.Next(tuple)) {
		os << "\t" << tuple;
	}

	os << "\t-----------------------------------------------------------\n";

	tuple.SetNull();

	return os;
}

//===--------------------------------------------------------------------===//
// Serialization/Deserialization
//===--------------------------------------------------------------------===//

bool Tile::SerializeTo(SerializeOutput &output, id_t num_tuples) {
	/**
	 * The table is serialized as:
	 *
	 * [(int) total size]
	 * [(int) header size] [num columns] [column types] [column names]
	 * [(int) num tuples] [tuple data]
	 *
	 */

	// A placeholder for the total table size written at the end
	std::size_t pos = output.Position();
	output.WriteInt(-1);

	// Serialize the header
	if (!SerializeHeaderTo(output))
		return false;

	// Active tuple count
	output.WriteInt(static_cast<int>(num_tuples));

	id_t written_count = 0;
	TileIterator tile_itr(this);
	Tuple tuple(schema);

	while (tile_itr.Next(tuple) && written_count < num_tuples) {
		tuple.SerializeTo(output);
		++written_count;
	}

	tuple.SetNull();

	assert(written_count == num_tuples);

	// Length prefix is non-inclusive
	int32_t sz = static_cast<int32_t>(output.Position() - pos - sizeof(int32_t));
	assert(sz > 0);
	output.WriteIntAt(pos, sz);

	return true;
}

bool Tile::SerializeHeaderTo(SerializeOutput &output) {
	std::size_t start;

	// Use the cache if possible
	if (column_header != NULL) {
		assert(column_header_size != INVALID_ID);
		output.WriteBytes(column_header, column_header_size);
		return true;
	}

	assert(column_header_size == INVALID_ID);

	// Skip header position
	start = output.Position();
	output.WriteInt(-1);

	// Status code
	output.WriteByte(-128);

	// Column counts as a short
	output.WriteShort(static_cast<int16_t>(column_count));

	// Write an array of column types as bytes
	for (id_t column_itr = 0; column_itr < column_count; ++column_itr) {
		ValueType type = schema->GetType(column_itr);
		output.WriteByte(static_cast<int8_t>(type));
	}

	// Write the array of column names as strings
	// NOTE: strings are ASCII only in metadata (UTF-8 in table storage)
	for (id_t column_itr = 0; column_itr < column_count; ++column_itr) {

		// Column name: Write (offset, length) for column definition, and string to string table
		const std::string& name = GetColumnName(column_itr);

		// Column names can't be null, so length must be >= 0
		int32_t length = static_cast<int32_t>(name.size());
		assert(length >= 0);

		// this is standard string serialization for voltdb
		output.WriteInt(length);
		output.WriteBytes(name.data(), length);
	}

	// Write the header size which is a non-inclusive int
	size_t Position = output.Position();
	column_header_size = static_cast<int32_t>(Position - start);

	int32_t non_inclusive_header_size = static_cast<int32_t>(column_header_size - sizeof(int32_t));
	output.WriteIntAt(start, non_inclusive_header_size);

	// Cache the column header
	column_header = new char[column_header_size];
	memcpy(column_header, static_cast<const char*>(output.Data()) + start, column_header_size);

	return true;
}

//  Serialized only the tuples specified, along with header.
bool Tile::SerializeTuplesTo(SerializeOutput &output, Tuple *tuples, int num_tuples) {
	std::size_t pos = output.Position();
	output.WriteInt(-1);

	assert(!tuples[0].IsNull());

	// Serialize the header
	if (!SerializeHeaderTo(output))
		return false;

	output.WriteInt(static_cast<int32_t>(num_tuples));
	for (int tuple_itr = 0; tuple_itr < num_tuples; tuple_itr++) {
		tuples[tuple_itr].SerializeTo(output);
	}

	// Length prefix is non-inclusive
	output.WriteIntAt(pos, static_cast<int32_t>(output.Position() - pos - sizeof(int32_t)));

	return true;
}

/**
 * Loads only tuple data, not schema, from the serialized tile.
 * Used for initial data loading.
 * @param allow_export if false, export enabled is overriden for this load.
 */
void Tile::DeserializeTuplesFrom(SerializeInput &input, Pool *pool) {
	/*
	 * Directly receives a Tile buffer.
	 * [00 01]   [02 03]   [04 .. 0x]
	 * rowstart  colcount  colcount * 1 byte (column types)
	 *
	 * [0x+1 .. 0y]
	 * colcount * strings (column names)
	 *
	 * [0y+1 0y+2 0y+3 0y+4]
	 * rowcount
	 *
	 * [0y+5 .. end]
	 * rowdata
	 */

	input.ReadInt(); // rowstart
	input.ReadByte();

	id_t column_count = input.ReadShort();
	assert(column_count > 0);

	// Store the following information so that we can provide them to the user on failure
	ValueType types[column_count];
	std::vector<std::string> names;

	// Skip the column types
	for (id_t column_itr = 0; column_itr < column_count; ++column_itr) {
		types[column_itr] = (ValueType) input.ReadEnumInSingleByte();
	}

	// Skip the column names
	for (id_t column_itr = 0; column_itr < column_count; ++column_itr) {
		names.push_back(input.ReadTextString());
	}

	// Check if the column count matches what the temp table is expecting
	if (column_count != schema->GetColumnCount()) {

		std::stringstream message(std::stringstream::in | std::stringstream::out);

		message << "Column count mismatch. Expecting "	<< schema->GetColumnCount()
																		<< ", but " << column_count << " given" << std::endl;
		message << "Expecting the following columns:" << std::endl;
		message << column_names.size() << std::endl;
		message << "The following columns are given:" << std::endl;

		for (id_t column_itr = 0; column_itr < column_count; column_itr++) {
			message << "column " << column_itr << ": " << names[column_itr] << ", type = "
					<< GetValueTypeName(types[column_itr]) << std::endl;
		}

		throw SerializationException(message.str());
	}

	// Use the deserialization routine skipping header
	DeserializeTuplesFromWithoutHeader(input, pool);
}

/**
 * Loads only tuple data and assumes there is no schema present.
 * Used for recovery where the schema is not sent.
 * @param allow_export if false, export enabled is overriden for this load.
 */
void Tile::DeserializeTuplesFromWithoutHeader(SerializeInput &input, Pool *pool) {
	id_t tuple_count = input.ReadInt();
	assert(tuple_count > 0);

	// First, check if we have required space
	assert(tuple_count <= num_tuple_slots);
	storage::Tuple *temp_tuple = new storage::Tuple(schema, true);

	for (id_t tuple_itr = 0; tuple_itr < tuple_count; ++tuple_itr) {
		temp_tuple->Move(GetTupleLocation(tuple_itr));
		temp_tuple->DeserializeFrom(input, pool);
		//TRACE("Loaded new tuple #%02d\n%s", tuple_itr, temp_target1.debug(Name()).c_str());
	}

}

//===--------------------------------------------------------------------===//
// Utilities
//===--------------------------------------------------------------------===//

// Compare two tiles (expensive !)
bool Tile::operator== (const Tile &other) const {
	if (!(GetColumnCount() == other.GetColumnCount()))
		return false;

	if (!(database_id == other.database_id))
		return false;

	catalog::Schema *other_schema = other.schema;
	if (*schema != *other_schema)
		return false;

	TileIterator tile_itr(this);
	TileIterator other_tile_itr(&other);

	Tuple tuple(schema);
	Tuple other_tuple(other_schema);

	while(tile_itr.Next(tuple)) {
		if (!(other_tile_itr.Next(other_tuple)))
			return false;

		if (!(tuple == other_tuple))
			return false;
	}

	tuple.SetNull();
	other_tuple.SetNull();

	return true;
}

bool Tile::operator!= (const Tile &other) const {
	return !(*this == other);
}

TileIterator Tile::GetIterator() {
	return TileIterator(this);
}

//TileStats* Tile::GetTileStats() {
//	return NULL;
//}


} // End storage namespace
} // End nstore namespace



