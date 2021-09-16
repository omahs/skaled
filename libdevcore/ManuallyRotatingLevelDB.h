#ifndef ROTATINGLEVELDB_H
#define ROTATINGLEVELDB_H

#include "LevelDB.h"

#include "batched_io.h"

#include <deque>
#include <set>
#include <shared_mutex>

namespace dev {
namespace db {

class batched_rotating_db_io : public batched_io::batched_face {
private:
    const boost::filesystem::path base_path;
    std::deque< std::unique_ptr< DatabaseFace > > pieces;
    size_t current_piece_file_no;

public:
    using const_iterator = std::deque< std::unique_ptr< DatabaseFace > >::const_iterator;
    batched_rotating_db_io( const boost::filesystem::path& _path, size_t _nPieces );
    const_iterator begin() const { return pieces.begin(); }
    const_iterator end() const { return pieces.end(); }
    DatabaseFace* current_piece() const { return begin()->get(); }
    size_t pieces_count() const {return  pieces.size();}
    void rotate();
    virtual void commit() {
        /*already implemented in rotate()*/
    }

protected:
    virtual void recover();
};

class ManuallyRotatingLevelDB : public DatabaseFace {
private:
    std::shared_ptr<batched_rotating_db_io> io_backend;

    mutable std::set< WriteBatchFace* > batch_cache;
    mutable std::shared_mutex m_mutex;

public:
    ManuallyRotatingLevelDB( std::shared_ptr<batched_rotating_db_io> _io_backend );
    void rotate();
    size_t piecesCount() const { return io_backend->pieces_count(); }

    virtual std::string lookup( Slice _key ) const;
    virtual bool exists( Slice _key ) const;
    virtual void insert( Slice _key, Slice _value );
    virtual void kill( Slice _key );

    virtual std::unique_ptr< WriteBatchFace > createWriteBatch() const;
    virtual void commit( std::unique_ptr< WriteBatchFace > _batch );
    // batches don't survive rotation!
    bool discardCreatedBatches() {
        std::unique_lock< std::shared_mutex > lock( m_mutex );
        size_t size = batch_cache.size();
        batch_cache.clear();
        return size > 0;
    }

    virtual void forEach( std::function< bool( Slice, Slice ) > f ) const;
    virtual h256 hashBase() const;
};

}  // namespace db
}  // namespace dev

#endif  // ROTATINGLEVELDB_H
