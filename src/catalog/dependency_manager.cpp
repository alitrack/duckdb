#include "duckdb/catalog/dependency_manager.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {
using namespace std;

DependencyManager::DependencyManager(Catalog &catalog) : catalog(catalog) {
}

void DependencyManager::AddObject(ClientContext &context, CatalogEntry *object,
                                  unordered_set<CatalogEntry *> &dependencies) {
	// check for each object in the sources if they were not deleted yet
	for (auto &dependency : dependencies) {
		idx_t entry_index;
		CatalogEntry *catalog_entry;
		if (!dependency->set->GetEntryInternal(context, dependency->name, entry_index, catalog_entry)) {
			throw InternalException("Dependency has already been deleted?");
		}
	}
	// add the object to the dependents_map of each object that it depends on
	for (auto &dependency : dependencies) {
		dependents_map[dependency].insert(object);
	}
	// create the dependents map for this object: it starts out empty
	dependents_map[object] = unordered_set<CatalogEntry *>();
	dependencies_map[object] = dependencies;
}

void DependencyManager::DropObject(ClientContext &context, CatalogEntry *object, bool cascade,
                                   set_lock_map_t &lock_set) {
	assert(dependents_map.find(object) != dependents_map.end());

	// first check the objects that depend on this object
	auto &dependent_objects = dependents_map[object];
	for (auto &dep : dependent_objects) {
		// look up the entry in the catalog set
		auto &catalog_set = *dep->set;
		idx_t entry_index;
		CatalogEntry *dependency_entry;

		if (!catalog_set.GetEntryInternal(context, dep->name, entry_index, dependency_entry)) {
			// the dependent object was already deleted, no conflict
			continue;
		}
		// conflict: attempting to delete this object but the dependent object still exists
		if (cascade) {
			// cascade: drop the dependent object
			catalog_set.DropEntryInternal(context, entry_index, *dependency_entry, cascade, lock_set);
		} else {
			// no cascade and there are objects that depend on this object: throw error
			throw CatalogException("Cannot drop entry \"%s\" because there are entries that "
			                       "depend on it. Use DROP...CASCADE to drop all dependents.",
			                       object->name);
		}
	}
}

void DependencyManager::AlterObject(ClientContext &context, CatalogEntry *old_obj, CatalogEntry *new_obj) {
	assert(dependents_map.find(old_obj) != dependents_map.end());
	assert(dependencies_map.find(old_obj) != dependencies_map.end());

	// first check the objects that depend on this object
	auto &dependent_objects = dependents_map[old_obj];
	for (auto &dep : dependent_objects) {
		// look up the entry in the catalog set
		auto &catalog_set = *dep->set;
		idx_t entry_index;
		CatalogEntry *dependency_entry;
		if (!catalog_set.GetEntryInternal(context, dep->name, entry_index, dependency_entry)) {
			// the dependent object was already deleted, no conflict
			continue;
		}
		// conflict: attempting to alter this object but the dependent object still exists
		// no cascade and there are objects that depend on this object: throw error
		throw CatalogException("Cannot alter entry \"%s\" because there are entries that "
		                       "depend on it.",
		                       old_obj->name);
	}
	// add the new object to the dependents_map of each object that it depents on
	auto &old_dependencies = dependencies_map[old_obj];
	for (auto &dependency : old_dependencies) {
		dependents_map[dependency].insert(new_obj);
	}
	// add the new object to the dependency manager
	dependents_map[new_obj] = unordered_set<CatalogEntry *>();
	dependencies_map[new_obj] = old_dependencies;
}

void DependencyManager::EraseObject(CatalogEntry *object) {
	// obtain the writing lock
	lock_guard<mutex> write_lock(catalog.write_lock);
	EraseObjectInternal(object);
}

void DependencyManager::EraseObjectInternal(CatalogEntry *object) {
	if (dependents_map.find(object) == dependents_map.end()) {
		// dependencies already removed
		return;
	}
	assert(dependents_map.find(object) != dependents_map.end());
	assert(dependencies_map.find(object) != dependencies_map.end());
	// now for each of the dependencies, erase the entries from the dependents_map
	for (auto &dependency : dependencies_map[object]) {
		auto entry = dependents_map.find(dependency);
		if (entry != dependents_map.end()) {
			assert(entry->second.find(object) != entry->second.end());
			entry->second.erase(object);
		}
	}
	// erase the dependents and dependencies for this object
	dependents_map.erase(object);
	dependencies_map.erase(object);
}

void DependencyManager::ClearDependencies(CatalogSet &set) {
	// obtain the writing lock
	lock_guard<mutex> write_lock(catalog.write_lock);

	// iterate over the objects in the CatalogSet
	for (auto &entry : set.entries) {
		CatalogEntry *centry = entry.second.get();
		while (centry) {
			EraseObjectInternal(centry);
			centry = centry->child.get();
		}
	}
}

} // namespace duckdb
