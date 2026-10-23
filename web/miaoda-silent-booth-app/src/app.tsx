import { Routes, Route } from "react-router-dom";
import { Layout } from "@/components/Layout";
import NotFoundPage from "@/pages/NotFoundPage/NotFoundPage";
import StatusPage from "@/pages/StatusPage/StatusPage";

export default function App() {
  return (
    <Routes>
      <Route element={<Layout />}>
        <Route index element={<StatusPage />} />
        <Route path="*" element={<NotFoundPage />} />
      </Route>
    </Routes>
  );
}
